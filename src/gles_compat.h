// gles_compat.h -- let GLQuake's desktop-OpenGL-1.1 renderer compile and run
// against the TouchPad's OpenGL ES 1.1.
//
// GLQuake is fixed-function, which maps almost perfectly onto GLES 1.1 -- the
// hard parts of a modern GL port (shaders, no matrix stack) simply do not
// arise. What GLES 1.1 removes and this header restores:
//
//   1. IMMEDIATE MODE. No glBegin/glVertex/glTexCoord/glColor/glEnd. Rather
//      than rewrite ~28 draw sites and 54 glVertex calls by hand -- tedious and
//      easy to get subtly wrong -- we reimplement immediate mode on top of
//      vertex arrays. Calls accumulate into buffers; glEnd() emits one
//      glDrawArrays. The renderer sources stay as id wrote them.
//   2. GL_QUADS / GL_POLYGON. Not in GLES. glEnd() converts: quads become two
//      triangles each, polygons become a triangle fan.
//   3. Entry points renamed or dropped: glOrtho->glOrthof, glFrustum->
//      glFrustumf, glDepthRange->glDepthRangef, and glDrawBuffer/glReadBuffer
//      which have no meaning here.
//
// Everything else GLQuake uses -- matrix stack, texturing, alpha test, fog-free
// blending, depth test, culling -- exists in GLES 1.1 unchanged.
#ifndef GLES_COMPAT_H
#define GLES_COMPAT_H

#include <GLES/gl.h>
#include <GLES/glext.h>

// ---------------------------------------------------------------------------
// GLES 1.1 has no double-precision GL types (the hardware is single precision
// throughout). GLQuake declares a few things GLdouble; alias them to float --
// nothing here needs the extra range, and glFrustumf/glOrthof take floats.
// ---------------------------------------------------------------------------
typedef GLfloat  GLdouble;
typedef GLfloat  GLclampd;

// ---------------------------------------------------------------------------
// Enums GLES 1.1 lacks. GL_QUADS/GL_POLYGON never reach the driver -- glEnd()
// translates them -- so any unused values will do.
// ---------------------------------------------------------------------------
#ifndef GL_QUADS
#define GL_QUADS        0x0007
#endif
#ifndef GL_POLYGON
#define GL_POLYGON      0x0009
#endif
#ifndef GL_QUAD_STRIP
#define GL_QUAD_STRIP   0x0008
#endif

// Referenced only by GLQuake's 8-bit paletted-texture upload path, which is
// dead here (VID_Is8bit() returns false -- the Adreno exposes
// GL_OES_compressed_paletted_texture, not the desktop colour-table extension).
// Defined so that code still compiles.
#ifndef GL_COLOR_INDEX
#define GL_COLOR_INDEX  0x1900
#endif
#ifndef GL_COLOR_INDEX8_EXT
#define GL_COLOR_INDEX8_EXT 0x80E5
#endif
#ifndef GL_SHARED_TEXTURE_PALETTE_EXT
#define GL_SHARED_TEXTURE_PALETTE_EXT 0x81FB
#endif

// Lightmap formats: GLES 1.1 has LUMINANCE/ALPHA/RGBA but not INTENSITY.
// GLQuake defaults to GL_LUMINANCE and only selects INTENSITY via "-lm_i", so
// this exists purely to keep the switch statements compiling. Its desktop value
// is used so it cannot collide with a real GLES enum.
#ifndef GL_INTENSITY
#define GL_INTENSITY    0x8049
#endif

// Desktop GL spellings of things GLES names differently.
#define glOrtho(l,r,b,t,n,f)   glOrthof((GLfloat)(l),(GLfloat)(r),(GLfloat)(b), \
                                        (GLfloat)(t),(GLfloat)(n),(GLfloat)(f))
#define glFrustum(l,r,b,t,n,f) glFrustumf((GLfloat)(l),(GLfloat)(r),(GLfloat)(b), \
                                          (GLfloat)(t),(GLfloat)(n),(GLfloat)(f))
#define glDepthRange(n,f)      glDepthRangef((GLfloat)(n),(GLfloat)(f))
#define glClearDepth(d)        glClearDepthf((GLfloat)(d))

// Single-buffer selection is meaningless with an EGL window surface.
#define glDrawBuffer(x)        ((void)0)
#define glReadBuffer(x)        ((void)0)

// GLES has no GL_ALPHA_TEST-less path issues; alpha test exists. But desktop
// code sometimes uses the double form.
#define glAlphaFunc(f,r)       glAlphaFunc((f),(GLclampf)(r))

// ---------------------------------------------------------------------------
// Immediate mode emulation. Same names as desktop GL so callers are unchanged.
// ---------------------------------------------------------------------------
void glesBegin(GLenum mode);
void glesEnd(void);
void glesVertex2f(GLfloat x, GLfloat y);
void glesVertex3f(GLfloat x, GLfloat y, GLfloat z);
void glesVertex3fv(const GLfloat *v);
void glesTexCoord2f(GLfloat s, GLfloat t);
void glesTexCoord2fv(const GLfloat *v);
void glesColor3f(GLfloat r, GLfloat g, GLfloat b);
void glesColor4f(GLfloat r, GLfloat g, GLfloat b, GLfloat a);
void glesColor3fv(const GLfloat *v);
void glesColor4fv(const GLfloat *v);
void glesColor3ubv(const GLubyte *v);
void glesColor4ub(GLubyte r, GLubyte g, GLubyte b, GLubyte a);
// Multitexture coordinate for the immediate-mode path (GL_ARB_multitexture in
// GLQuake; native in GLES 1.1 as glMultiTexCoord4f).
void glesMultiTexCoord2f(GLenum target, GLfloat s, GLfloat t);

#define glBegin            glesBegin
#define glEnd              glesEnd
#define glVertex2f         glesVertex2f
#define glVertex3f         glesVertex3f
#define glVertex3fv        glesVertex3fv
#define glTexCoord2f       glesTexCoord2f
#define glTexCoord2fv      glesTexCoord2fv
#define glColor3f          glesColor3f
#define glColor4f          glesColor4f
#define glColor3fv         glesColor3fv
#define glColor4fv         glesColor4fv
#define glColor3ubv        glesColor3ubv
#define glColor4ub         glesColor4ub

#endif // GLES_COMPAT_H
