// gles_compat.c -- immediate-mode emulation over OpenGL ES 1.1 vertex arrays.
// See gles_compat.h for why this exists.
//
// Design notes:
//  * One shared set of static arrays, sized for the largest batch GLQuake ever
//    submits between a glBegin/glEnd pair (sky/warp surfaces and alias models
//    are the big ones). Overflow is clamped rather than reallocating, so a
//    pathological batch drops geometry instead of crashing -- and warns once.
//  * Colour and texcoord arrays are only enabled if the batch actually set
//    them. GLQuake frequently draws with just vertices (relying on the current
//    glColor), so always enabling them would both cost bandwidth and, for
//    colour, override the fixed-function current colour with stale data.
//  * GL_QUADS is expanded to triangles on emit. GLQuake's quads are all convex
//    and in 0,1,2,3 order, so 0-1-2 + 0-2-3 is correct.
#include "quakedef.h"

#ifdef __webos__

#include "gles_compat.h"

// Undo the header's redirections so we can define the real entry points.
#undef glBegin
#undef glEnd
#undef glVertex2f
#undef glVertex3f
#undef glVertex3fv
#undef glTexCoord2f
#undef glTexCoord2fv
#undef glColor3f
#undef glColor4f
#undef glColor3fv
#undef glColor4fv
#undef glColor3ubv
#undef glColor4ub

// Sized for the largest batch GLQuake submits between one glBegin/glEnd. 4096
// was not enough -- world/sky surfaces overflowed it and dropped geometry.
#define IM_MAX_VERTS 16384

static GLfloat  im_vert[IM_MAX_VERTS * 3];
static GLfloat  im_tex0[IM_MAX_VERTS * 2];
static GLfloat  im_tex1[IM_MAX_VERTS * 2];
static GLubyte  im_col [IM_MAX_VERTS * 4];

static int      im_count;          // vertices accumulated this batch
static GLenum   im_mode;           // primitive passed to glBegin
static int      im_active;
static int      im_use_tex0, im_use_tex1, im_use_col;
static int      im_overflowed;

// "Current" attributes, sticky across vertices exactly like desktop GL.
static GLfloat  cur_s0, cur_t0, cur_s1, cur_t1;
static GLubyte  cur_r = 255, cur_g = 255, cur_b = 255, cur_a = 255;

// Scratch for quad -> triangle expansion (4 verts -> 6).
static GLfloat  ex_vert[IM_MAX_VERTS * 3 / 4 * 6 + 6];
static GLfloat  ex_tex0[IM_MAX_VERTS * 2 / 4 * 6 + 6];
static GLfloat  ex_tex1[IM_MAX_VERTS * 2 / 4 * 6 + 6];
static GLubyte  ex_col [IM_MAX_VERTS * 4 / 4 * 6 + 6];

void glesBegin(GLenum mode)
{
    im_mode     = mode;
    im_count    = 0;
    im_active   = 1;
    im_use_tex0 = im_use_tex1 = im_use_col = 0;
}

static void im_put(GLfloat x, GLfloat y, GLfloat z)
{
    int i;
    if (im_count >= IM_MAX_VERTS) {
        if (!im_overflowed) {
            im_overflowed = 1;
            Con_Printf("gles: immediate batch overflow (>%d verts) -- "
                       "geometry dropped\n", IM_MAX_VERTS);
        }
        return;
    }
    i = im_count;
    im_vert[i * 3 + 0] = x;
    im_vert[i * 3 + 1] = y;
    im_vert[i * 3 + 2] = z;
    im_tex0[i * 2 + 0] = cur_s0;  im_tex0[i * 2 + 1] = cur_t0;
    im_tex1[i * 2 + 0] = cur_s1;  im_tex1[i * 2 + 1] = cur_t1;
    im_col [i * 4 + 0] = cur_r;   im_col [i * 4 + 1] = cur_g;
    im_col [i * 4 + 2] = cur_b;   im_col [i * 4 + 3] = cur_a;
    im_count++;
}

void glesVertex2f(GLfloat x, GLfloat y)          { im_put(x, y, 0.0f); }
void glesVertex3f(GLfloat x, GLfloat y, GLfloat z) { im_put(x, y, z); }
void glesVertex3fv(const GLfloat *v)             { im_put(v[0], v[1], v[2]); }

void glesTexCoord2f(GLfloat s, GLfloat t)
{
    cur_s0 = s; cur_t0 = t; im_use_tex0 = 1;
}
void glesTexCoord2fv(const GLfloat *v) { glesTexCoord2f(v[0], v[1]); }

void glesMultiTexCoord2f(GLenum target, GLfloat s, GLfloat t)
{
    if (target == GL_TEXTURE1) { cur_s1 = s; cur_t1 = t; im_use_tex1 = 1; }
    else                       { cur_s0 = s; cur_t0 = t; im_use_tex0 = 1; }
}

static void im_setcol(GLfloat r, GLfloat g, GLfloat b, GLfloat a)
{
    cur_r = (GLubyte)(r * 255.0f); cur_g = (GLubyte)(g * 255.0f);
    cur_b = (GLubyte)(b * 255.0f); cur_a = (GLubyte)(a * 255.0f);
    if (im_active) im_use_col = 1;
    else glColor4f(r, g, b, a);      // outside a batch: real current colour
}

void glesColor3f(GLfloat r, GLfloat g, GLfloat b)            { im_setcol(r,g,b,1.0f); }
void glesColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a) { im_setcol(r,g,b,a); }
void glesColor3fv(const GLfloat *v)                          { im_setcol(v[0],v[1],v[2],1.0f); }
void glesColor4fv(const GLfloat *v)                          { im_setcol(v[0],v[1],v[2],v[3]); }
void glesColor3ubv(const GLubyte *v)
{
    im_setcol(v[0] / 255.0f, v[1] / 255.0f, v[2] / 255.0f, 1.0f);
}
void glesColor4ub(GLubyte r, GLubyte g, GLubyte b, GLubyte a)
{
    im_setcol(r / 255.0f, g / 255.0f, b / 255.0f, a / 255.0f);
}

// Expand N quads (4n verts) into 2N triangles (6n verts): 0-1-2, 0-2-3.
static int im_expand_quads(void)
{
    int q, nq = im_count / 4, o = 0;
    static const int idx[6] = { 0, 1, 2, 0, 2, 3 };
    for (q = 0; q < nq; q++) {
        int k;
        for (k = 0; k < 6; k++) {
            int src = q * 4 + idx[k];
            ex_vert[o * 3 + 0] = im_vert[src * 3 + 0];
            ex_vert[o * 3 + 1] = im_vert[src * 3 + 1];
            ex_vert[o * 3 + 2] = im_vert[src * 3 + 2];
            ex_tex0[o * 2 + 0] = im_tex0[src * 2 + 0];
            ex_tex0[o * 2 + 1] = im_tex0[src * 2 + 1];
            ex_tex1[o * 2 + 0] = im_tex1[src * 2 + 0];
            ex_tex1[o * 2 + 1] = im_tex1[src * 2 + 1];
            ex_col [o * 4 + 0] = im_col [src * 4 + 0];
            ex_col [o * 4 + 1] = im_col [src * 4 + 1];
            ex_col [o * 4 + 2] = im_col [src * 4 + 2];
            ex_col [o * 4 + 3] = im_col [src * 4 + 3];
            o++;
        }
    }
    return o;
}

void glesEnd(void)
{
    const GLfloat *vp = im_vert, *t0 = im_tex0, *t1 = im_tex1;
    const GLubyte *cp = im_col;
    GLenum mode = im_mode;
    int n = im_count;

    im_active = 0;
    if (n < 1) return;

    switch (mode) {
    case GL_QUADS:
        n    = im_expand_quads();
        mode = GL_TRIANGLES;
        vp = ex_vert; t0 = ex_tex0; t1 = ex_tex1; cp = ex_col;
        break;
    case GL_POLYGON:
    case GL_QUAD_STRIP:
        // A convex polygon is a triangle fan; a quad strip is a triangle strip
        // (its vertex order already alternates correctly).
        mode = (mode == GL_POLYGON) ? GL_TRIANGLE_FAN : GL_TRIANGLE_STRIP;
        break;
    default:
        break;                       // POINTS/LINES/TRIANGLES/FAN/STRIP pass through
    }

    glEnableClientState(GL_VERTEX_ARRAY);
    glVertexPointer(3, GL_FLOAT, 0, vp);

    if (im_use_tex0) {
        glClientActiveTexture(GL_TEXTURE0);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        glTexCoordPointer(2, GL_FLOAT, 0, t0);
    }
    if (im_use_tex1) {
        glClientActiveTexture(GL_TEXTURE1);
        glEnableClientState(GL_TEXTURE_COORD_ARRAY);
        glTexCoordPointer(2, GL_FLOAT, 0, t1);
        glClientActiveTexture(GL_TEXTURE0);
    }
    if (im_use_col) {
        glEnableClientState(GL_COLOR_ARRAY);
        glColorPointer(4, GL_UNSIGNED_BYTE, 0, cp);
    }

    glDrawArrays(mode, 0, n);

    if (im_use_col) glDisableClientState(GL_COLOR_ARRAY);
    if (im_use_tex1) {
        glClientActiveTexture(GL_TEXTURE1);
        glDisableClientState(GL_TEXTURE_COORD_ARRAY);
        glClientActiveTexture(GL_TEXTURE0);
    }
    if (im_use_tex0) glDisableClientState(GL_TEXTURE_COORD_ARRAY);
    glDisableClientState(GL_VERTEX_ARRAY);

    // Restore the fixed-function current colour: a colour array overrides it
    // while enabled, and GLQuake assumes the last glColor sticks afterwards.
    if (im_use_col)
        glColor4f(cur_r / 255.0f, cur_g / 255.0f, cur_b / 255.0f, cur_a / 255.0f);

    im_count = 0;
}

#endif // __webos__
