// gl_vidsdl.c -- OpenGL ES 1.1 video backend for the TouchPad.
//
// Replaces the software renderer's SDL surface path with a GL context. The
// input half of this file -- the touch overlay gestures, the SDL event pump and
// the evdev controller/keyboard polling -- is unchanged from vid_sdl.c, since
// none of it depends on how pixels reach the screen.
//
// THE CONTEXT INCANTATION (this cost a day; do not "simplify" it):
// Palm's SDL 1.2 defaults to requesting an OpenGL ES *2* context, and this
// Adreno driver fails that with EGL_BAD_ALLOC -- surfaced only as SDL's
// unhelpful "Could not create EGL context". Setting CONTEXT_MAJOR_VERSION to 1
// makes SDL drop EGL_RENDERABLE_TYPE=ES2_BIT from the config request and pass
// NULL context attributes, which is exactly what the known-good GLES app on
// this device (Tux Racer) does, and the context then allocates first try.
// Also: SDL must own the context. Never call EGL directly -- that breaks the
// 3-layer compositor and makes the screen flicker on every touch.

#include "SDL.h"
#include "quakedef.h"
#include "glquake.h"
#include "gles_compat.h"
#include "pdl.h"
#include "in_evdev.h"

#define    BASEWIDTH    1024
#define    BASEHEIGHT   768

#define    FIRE_SIZE    160
#define    JUMP_SIZE    120
#define    JOY_SIZE     160
#define    JOY_DEAD     10
#define    JOY_X        80
#define    JOY_Y        ( (float)vid.height - 80.0f )

#define    OVERLAY_ITEM_COUNT   4

viddef_t    vid;                // global video state
unsigned short  d_8to16table[256];

byte    autofire = 0;
byte    mousedown = 0;
byte    normalkeyboard = 0;
byte    drawoverlay = 1;
byte    gesturedown = 0;
extern int in_impulse;

int     jumping_counter = 0;
#define JUMP_FRAME_COUNT 6
int     fire_counter = 0;
#define FIRE_FRAME_COUNT 1

int    VGA_width, VGA_height, VGA_rowbytes, VGA_bufferrowbytes = 0;
byte    *VGA_pagebase;

static SDL_Surface *screen = NULL;

static qboolean mouse_avail;
static float   mouse_x, mouse_y;
static float   joy_x, joy_y;

void (*vid_menudrawfn)(void) = NULL;
void (*vid_menukeyfn)(int key) = NULL;

// GL renderer globals (declared extern in glquake.h; the desktop backends
// define them, so this one must too).
static int   scr_width, scr_height;
qboolean     isPermedia = false;
qboolean     gl_mtexable = false;

unsigned char   d_15to8table[65536];
int             texture_mode = GL_LINEAR;
int             texture_extension_number = 1;
float           gldepthmin, gldepthmax;
cvar_t          gl_ztrick = {"gl_ztrick", "1"};
const char     *gl_vendor;
const char     *gl_renderer;
const char     *gl_version;
const char     *gl_extensions;

// GLQuake resolves multitexture through these; GLES 1.1 has the calls natively,
// so point them straight at it. glesMultiTexCoord2f feeds the immediate-mode
// buffers (see gles_compat.c) rather than the driver, because coordinates
// inside a glBegin/glEnd batch must land in the vertex arrays.
static void GLES_SelectTexture (GLenum target)
{
    glActiveTexture(target);
    glClientActiveTexture(target);
}
static void GLES_MTexCoord2f (GLenum target, GLfloat s, GLfloat t)
{
    glesMultiTexCoord2f(target, s, t);
}

/*
=================
VID_Init
=================
*/
void    VID_Init (unsigned char *palette)
{
    int pnum;

    // PDL first, then orientation, then SDL -- the compositor decides the
    // window surface's geometry when the GL context is created, so anything
    // that affects it has to happen before SDL_SetVideoMode.
    if (PDL_Init(0) != PDL_NOERROR)
        Con_Printf("PDL_Init failed (continuing)\n");
    // Deliberately NO PDL_SetOrientation: PDK apps are landscape by default,
    // and the known-good GLES reference on this device never calls it. Asking
    // for an orientation appears to hand back a rotated phone-geometry card.

    if (SDL_Init(SDL_INIT_VIDEO | SDL_INIT_AUDIO) < 0)
        Sys_Error("VID: Couldn't load SDL: %s", SDL_GetError());

    // Ask EXACTLY what the known-good GLES app on this device asks for. Two
    // things matter and both were learned by LD_PRELOAD-logging Tux Racer:
    //
    //  1. CONTEXT_MAJOR/MINOR_VERSION = 1. Palm's SDL otherwise requests an
    //     OpenGL ES *2* context, which this Adreno driver refuses with
    //     EGL_BAD_ALLOC -- reported only as "Could not create EGL context".
    //  2. Size 0x0 with plain SDL_OPENGL and NO fullscreen flag. Asking for an
    //     explicit size (or passing SDL_OPENGLES/SDL_FULLSCREEN ourselves) gets
    //     a 320x480 Palm-Pre-sized surface; desktop mode gets the panel's
    //     native 1024x768. SDL adds the fullscreen and GLES flags itself.
    //
    // Everything else -- colour depth, depth buffer -- is left at SDL's
    // defaults, which resolve to RGB 5/6/5 with a 16-bit depth buffer.
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 1);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);

    screen = SDL_SetVideoMode(0, 0, 0, SDL_OPENGL);
    if (!screen)
        Sys_Error("VID: Couldn't set GL video mode: %s\n", SDL_GetError());

    // Render at whatever the compositor actually gave us, rather than assuming.
    // -winsize still overrides, for experimenting with lower internal
    // resolutions.
    vid.width  = vid.conwidth  = screen->w;
    vid.height = vid.conheight = screen->h;
    if ((pnum = COM_CheckParm("-winsize")) && pnum < com_argc - 2) {
        int w = Q_atoi(com_argv[pnum + 1]), h = Q_atoi(com_argv[pnum + 2]);
        if (w > 0 && h > 0) {
            vid.width = vid.conwidth = w;
            vid.height = vid.conheight = h;
        }
    }
    scr_width  = vid.width;
    scr_height = vid.height;
    glViewport(0, 0, vid.width, vid.height);

    PDL_CustomPauseUiEnable(PDL_FALSE);

    // Report what the compositor ACTUALLY gave us. The card is portrait
    // (LunaSysMgr logs 768x1024), so if the surface comes back portrait while
    // we render assuming landscape, the picture is sideways.
    {
        GLint vp[4] = {0,0,0,0};
        glGetIntegerv(GL_VIEWPORT, vp);
        Con_Printf("GL surface: SDL says %dx%d, viewport %dx%d, wanted %dx%d\n",
                   screen->w, screen->h, vp[2], vp[3], vid.width, vid.height);
    }

    vid.aspect = ((float)vid.height / (float)vid.width) * (320.0 / 240.0);
    vid.numpages = 2;
    vid.colormap = host_colormap;
    vid.fullbright = 256 - LittleLong(*((int *)vid.colormap + 2048));

    VGA_width  = vid.conwidth;
    VGA_height = vid.conheight;

    GL_Init();

    // Build the 24-bit palette the GL renderer uses for texture uploads.
    VID_SetPalette(palette);

    Con_Printf("Video mode %dx%d initialised (OpenGL ES)\n", vid.width, vid.height);
    vid.recalc_refdef = 1;
}

void    VID_Shutdown (void)
{
    SDL_Quit();
}

/*
=================
Palette

The GL renderer works from d_8to24table, built here from Quake's palette
lump; there is no hardware palette to set.
=================
*/
unsigned    d_8to24table[256];

void    VID_SetPalette (unsigned char *palette)
{
    int      i;
    byte    *pal = palette;
    unsigned *table = d_8to24table;

    for (i = 0; i < 256; i++) {
        unsigned r = pal[0], g = pal[1], b = pal[2];
        pal += 3;
        *table++ = (255 << 24) | (b << 16) | (g << 8) | r;
    }
    d_8to24table[255] &= 0x00ffffff;      // 255 is transparent
}

void    VID_ShiftPalette (unsigned char *palette)
{
    // GL applies gamma/blends in the renderer; nothing to do here.
}

qboolean VID_Is8bit (void) { return false; }

/*
=================
GL_Init
=================
*/
void GL_Init (void)
{
    gl_vendor     = (char *)glGetString(GL_VENDOR);
    gl_renderer   = (char *)glGetString(GL_RENDERER);
    gl_version    = (char *)glGetString(GL_VERSION);
    gl_extensions = (char *)glGetString(GL_EXTENSIONS);
    Con_Printf("GL_VENDOR: %s\n", gl_vendor);
    Con_Printf("GL_RENDERER: %s\n", gl_renderer);
    Con_Printf("GL_VERSION: %s\n", gl_version);

    // GLES 1.1 has multitexture natively; GLQuake reaches it through function
    // pointers it normally resolves as an extension.
    {
        GLint units = 0;
        glGetIntegerv(GL_MAX_TEXTURE_UNITS, &units);
        gl_mtexable = (units >= 2) && !COM_CheckParm("-nomtex");
        // gl_rsurf.c owns these pointers; wire them to the native GLES calls.
        qglMTexCoord2fSGIS   = GLES_MTexCoord2f;
        qglSelectTextureSGIS = GLES_SelectTexture;
        Con_Printf("Texture units: %d, multitexture %s\n", units,
                   gl_mtexable ? "enabled" : "disabled");
    }

    glClearColor(0, 0, 0, 0);
    glCullFace(GL_FRONT);
    glEnable(GL_TEXTURE_2D);

    glEnable(GL_ALPHA_TEST);
    glAlphaFunc(GL_GREATER, 0.666f);

    // No glPolygonMode in GLES -- filled is the only mode, which is what
    // GLQuake asks for anyway.
    glShadeModel(GL_FLAT);

    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MIN_FILTER, GL_NEAREST);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_MAG_FILTER, GL_NEAREST);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_S, GL_REPEAT);
    glTexParameterf(GL_TEXTURE_2D, GL_TEXTURE_WRAP_T, GL_REPEAT);

    glBlendFunc(GL_SRC_ALPHA, GL_ONE_MINUS_SRC_ALPHA);
    glTexEnvf(GL_TEXTURE_ENV, GL_TEXTURE_ENV_MODE, GL_REPLACE);
}

void GL_BeginRendering (int *x, int *y, int *width, int *height)
{
    *x = *y = 0;
    *width  = scr_width;
    *height = scr_height;
}

void GL_EndRendering (void)
{
    glFlush();
    SDL_GL_SwapBuffers();
}

// The software renderer punched loading-plaque pixels straight into the
// framebuffer; there is no such buffer here.
void D_BeginDirectRect (int x, int y, byte *pbitmap, int width, int height) { }
void D_EndDirectRect (int x, int y, int width, int height) { }

/* ===========================================================================
 * Menu-aware touch overlay
 *   Quake menus only act on K_ENTER / K_ESCAPE / arrow keys. The on-screen
 *   overlay normally sends K_MOUSE1 (fire) + analog joystick, none of which a
 *   menu understands -- so a touch-only player could reach the menu but never
 *   SELECT anything. When key_dest isn't key_game we reinterpret the overlay:
 *     FIRE button   -> Enter (select)
 *     JUMP / top    -> Escape (back / open-close menu)
 *     joystick drag -> one arrow-key step per push (menu navigation)
 * ======================================================================== */
static int menu_btn_key = 0;    // menu key currently held by a touch button
static int menu_joy_dir = 0;    // last arrow emitted by the joystick-as-dpad

static void Menu_TouchDown( int x, int y )
{
    int key = 0;
    if ( x > vid.width - FIRE_SIZE && y > vid.height - FIRE_SIZE )
        key = K_ENTER;                 // fire button -> select
    else if ( y < JUMP_SIZE )
        key = K_ESCAPE;                // top / jump  -> back
    if ( key ) { menu_btn_key = key; Key_Event( key, true ); }
}

static void Menu_TouchUp( void )
{
    if ( menu_btn_key ) { Key_Event( menu_btn_key, false ); menu_btn_key = 0; }
}

// Treat the on-screen joystick as a d-pad: emit one arrow step when the drag
// enters a new direction, re-arming only after it returns toward center.
static void Menu_JoyStep( int x, int y )
{
    int cx  = JOY_X;
    int cy  = (int)JOY_Y;
    int dx  = x - cx, dy = y - cy;
    int adx = dx < 0 ? -dx : dx;
    int ady = dy < 0 ? -dy : dy;
    int dir = 0;

    if ( adx > JOY_DEAD * 3 || ady > JOY_DEAD * 3 )
    {
        if ( ady >= adx ) dir = ( dy < 0 ) ? K_UPARROW : K_DOWNARROW;
        else              dir = ( dx < 0 ) ? K_LEFTARROW : K_RIGHTARROW;
    }
    if ( dir != menu_joy_dir )
    {
        if ( dir ) { Key_Event( dir, true ); Key_Event( dir, false ); }
        menu_joy_dir = dir;
    }
}

/*
================
Sys_SendKeyEvents
================
*/

void Sys_SendKeyEvents(void)
{
    SDL_Event event;
    int sym, state;
     int modstate;

    while (SDL_PollEvent(&event))
    {
        switch (event.type) {

            case SDL_KEYDOWN:
            case SDL_KEYUP:
                sym = event.key.keysym.sym;
                state = event.key.state;
                modstate = SDL_GetModState();
                switch(sym)
                {
                   case SDLK_DELETE: sym = K_DEL; break;
                   case SDLK_BACKSPACE: sym = K_BACKSPACE; break;
                   case SDLK_F1: sym = K_F1; break;
                   case SDLK_F2: sym = K_F2; break;
                   case SDLK_F3: sym = K_F3; break;
                   case SDLK_F4: sym = K_F4; break;
                   case SDLK_F5: sym = K_F5; break;
                   case SDLK_F6: sym = K_F6; break;
                   case SDLK_F7: sym = K_F7; break;
                   case SDLK_F8: sym = K_F8; break;
                   case SDLK_F9: sym = K_F9; break;
                   case SDLK_F10: sym = K_F10; break;
                   case SDLK_F11: sym = K_F11; break;
                   case SDLK_F12: sym = K_F12; break;
                   case SDLK_BREAK:
                   case SDLK_PAUSE: sym = K_PAUSE; break;
                   case SDLK_UP: sym = K_UPARROW; break;
                   case SDLK_DOWN: sym = K_DOWNARROW; break;
                   case SDLK_RIGHT: sym = K_RIGHTARROW; break;
                   case SDLK_LEFT: sym = K_LEFTARROW; break;
                   case SDLK_INSERT: sym = K_INS; break;
                   case SDLK_HOME: sym = K_HOME; break;
                   case SDLK_END: sym = K_END; break;
                   case SDLK_PAGEUP: sym = K_PGUP; break;
                   case SDLK_PAGEDOWN: sym = K_PGDN; break;
                   case SDLK_RSHIFT:
                   case SDLK_LSHIFT: sym = K_SHIFT; break;
                   case SDLK_RCTRL:
                   case SDLK_LCTRL: sym = K_CTRL; break;
                   case SDLK_RALT:
                   case SDLK_LALT: sym = K_ALT; break;
                   case SDLK_KP0: 
                       if(modstate & KMOD_NUM) sym = K_INS; 
                       else sym = SDLK_0;
                       break;
                   case SDLK_KP1:
                       if(modstate & KMOD_NUM) sym = K_END;
                       else sym = SDLK_1;
                       break;
                   case SDLK_KP2:
                       if(modstate & KMOD_NUM) sym = K_DOWNARROW;
                       else sym = SDLK_2;
                       break;
                   case SDLK_KP3:
                       if(modstate & KMOD_NUM) sym = K_PGDN;
                       else sym = SDLK_3;
                       break;
                   case SDLK_KP4:
                       if(modstate & KMOD_NUM) sym = K_LEFTARROW;
                       else sym = SDLK_4;
                       break;
                   case SDLK_KP5: sym = SDLK_5; break;
                   case SDLK_KP6:
                       if(modstate & KMOD_NUM) sym = K_RIGHTARROW;
                       else sym = SDLK_6;
                       break;
                   case SDLK_KP7:
                       if(modstate & KMOD_NUM) sym = K_HOME;
                       else sym = SDLK_7;
                       break;
                   case SDLK_KP8:
                       if(modstate & KMOD_NUM) sym = K_UPARROW;
                       else sym = SDLK_8;
                       break;
                   case SDLK_KP9:
                       if(modstate & KMOD_NUM) sym = K_PGUP;
                       else sym = SDLK_9;
                       break;
                   case SDLK_KP_PERIOD:
                       if(modstate & KMOD_NUM) sym = K_DEL;
                       else sym = SDLK_PERIOD;
                       break;
                   case SDLK_KP_DIVIDE: sym = SDLK_SLASH; break;
                   case SDLK_KP_MULTIPLY: sym = SDLK_ASTERISK; break;
                   case SDLK_KP_MINUS: sym = SDLK_MINUS; break;
                   case SDLK_KP_PLUS: sym = SDLK_PLUS; break;
                   case SDLK_KP_ENTER: sym = SDLK_RETURN; break;
                   case SDLK_KP_EQUALS: sym = SDLK_EQUALS; break;
                }
                // If we're not directly handled and still above 255
                // just force it to 0
                if(sym > 255) sym = 0;

                //XXX: This is a terrible hack
                // because for some reason
                // parsing configs doesn't work?

                //sym+i is tilde
                if ( sym == 37 ) sym = SDLK_BACKQUOTE;

                if ( !normalkeyboard )
                {
                    //these values are from keys.h
                    if ( sym == SDLK_j ) sym = K_CTRL;//fire!
                    if ( sym == SDLK_b ) sym = K_SPACE;//jump!
                    //if ( sym == SDLK_j ) sym = K_UPARROW;//forward!
                    //if ( sym == SDLK_b ) sym = K_DOWNARROW;//back
                    if ( sym == SDLK_h ) sym = 44;//strafeleft
                    if ( sym == SDLK_n ) sym = 46;//straferight

                    //same, only sprint versions
                    if ( sym == SDLK_i ) 
                    {
                        Key_Event( K_SHIFT, state );
                        Key_Event( K_UPARROW, state );
                        Key_Event( K_SHIFT, state );
                        sym = 0;
                    }
                    if ( sym == SDLK_u ) 
                    {
                        Key_Event( K_SHIFT, state );
                        Key_Event( 44, state );
                        Key_Event( K_SHIFT, state );
                        sym = 0;
                    }
                    if ( sym == SDLK_k ) 
                    {
                        Key_Event( K_SHIFT, state );
                        Key_Event( 46, state );
                        Key_Event( K_SHIFT, state );
                        sym = 0;
                    }

                    //remap the numbers  to the weapons, so no orange needed
                    if ( sym == SDLK_e ) sym = SDLK_1;
                    if ( sym == SDLK_r ) sym = SDLK_2;
                    if ( sym == SDLK_t ) sym = SDLK_3;
                    if ( sym == SDLK_d ) sym = SDLK_4;
                    if ( sym == SDLK_f ) sym = SDLK_5;
                    if ( sym == SDLK_g ) sym = SDLK_6;
                    if ( sym == SDLK_x ) sym = SDLK_7;
                    if ( sym == SDLK_c ) sym = SDLK_8;
                    if ( sym == SDLK_v ) sym = SDLK_9;

                    //quick load/quick save
                    if ( sym == SDLK_QUOTE ) sym = K_F9;//load
                    if ( sym == SDLK_UNDERSCORE ) sym = K_F6;//save

                    //menu
                    if ( sym == SDLK_q ) sym = K_ESCAPE;

                    //arrow keys for menu nav
                    if ( sym == SDLK_w ) sym = K_LEFTARROW;
                    if ( sym == SDLK_s ) sym = K_UPARROW;
                    if ( sym == SDLK_z ) sym = K_RIGHTARROW;
                    if ( sym == SDLK_a ) sym = K_DOWNARROW;

                    if ( sym == SDLK_0 && state )
                    {
                        drawoverlay = !drawoverlay;
                        if ( drawoverlay )
                        {
                            Con_Printf( "Overlay enabled. Press orange+'@' (0) to toggle back.\n" );
                        }
                        else
                        {
                            Con_Printf( "Overlay disabled. Press orange+'@' (0) to toggle back.\n" );
                        }
                    }
                }

                //Weapon cycling!

                //gesture down
                //here we use the full name since in normal keyboard mode
                //we bind 'q' to escape, same as swipe down
                if ( event.key.keysym.sym == 27 && state )
                {
                    in_impulse = 10;
                    break;
                }

                //gesture up
                if ( sym == 229 && state )
                {
                    in_impulse = 12;
                    break;
                }

                //gesture button
                if ( sym == 231 )
                {
                    gesturedown = state;
                }

                if ( sym == SDLK_AT && state )
                {
                    normalkeyboard = !normalkeyboard;
                    if ( normalkeyboard )
                    {
                        Con_Printf( "Normal keyboard enabled. Press '@' to toggle back.\n" );
                    }
                    else
                    {
                        Con_Printf( "Action keyboard enabled. Press '@' to toggle back.\n" );
                    }
                }

                Key_Event(sym, state);
                break;

            case SDL_MOUSEBUTTONUP:
                if ( key_dest != key_game )
                {
                    Menu_TouchUp();
                    menu_joy_dir = 0;
                    break;
                }
                if ( event.motion.y > vid.height - JOY_SIZE &&
                     event.motion.x < JOY_SIZE )
                {
                    joy_x = joy_y = 0;
                    mousedown = false;
                }

                //fall through
            case SDL_MOUSEBUTTONDOWN:

                if ( key_dest != key_game )
                {
                    if ( event.type == SDL_MOUSEBUTTONDOWN )
                        Menu_TouchDown( event.button.x, event.button.y );
                    break;
                }

                if ( event.motion.x > vid.width - FIRE_SIZE &&
                        event.motion.y > vid.height - FIRE_SIZE )
                {
                    //FIRE!
                    Key_Event( K_MOUSE1, event.button.state );
                    autofire = event.button.state;
                    if ( autofire )
                    {
                        fire_counter = FIRE_FRAME_COUNT;
                    }
                    break;
                }
                break;
            case SDL_MOUSEMOTION:
                //printf( "MOUSE: %d, %d\n", event.motion.xrel, event.motion.yrel );

                if ( key_dest != key_game )
                {
                    // joystick corner acts as a menu d-pad
                    if ( event.motion.y > vid.height - JOY_SIZE &&
                         event.motion.x < JOY_SIZE )
                        Menu_JoyStep( event.motion.x, event.motion.y );
                    break;
                }

                if ( mousedown &&
                        event.motion.y > vid.height - JOY_SIZE &&
                        event.motion.x < JOY_SIZE )
                {
                    joy_x = ( event.motion.x - JOY_X );
                    if ( joy_x < JOY_DEAD && joy_x > -JOY_DEAD )
                    {
                        joy_x = 0;
                    }
                    else
                    {
                        if ( joy_x >= JOY_DEAD )
                        {
                            joy_x -= JOY_DEAD;
                        }
                        else
                        {
                            joy_x += JOY_DEAD;
                        }
                    }

                    joy_y = -( JOY_Y - event.motion.y );
                    
                    joy_y = ( event.motion.y - JOY_Y );
                    if ( joy_y < JOY_DEAD && joy_y > -JOY_DEAD )
                    {
                        joy_y = 0;
                    }
                    else
                    {
                        if ( joy_y >= JOY_DEAD )
                        {
                            joy_y -= JOY_DEAD;
                        }
                        else
                        {
                            joy_y += JOY_DEAD;
                        }
                    }
                    break;
                }

                if ( !mousedown )
                {
                    joy_x = 0;
                    joy_y = 0;
                }

                //jump: top
                if ( event.motion.y < JUMP_SIZE )
                {
                    //top-left corner, jump!
                    jumping_counter = JUMP_FRAME_COUNT;
                    Key_Event( 32, true );
                    Key_Event( 32, false );
                    break;
                }
                break;

            case SDL_QUIT:
                CL_Disconnect ();
                Host_ShutdownServer(false);
                Sys_Quit ();
                break;
            default:
                break;
        }

    }

#ifdef __webos__
    // Fold in USB/Bluetooth controllers and physical keyboards (direct evdev);
    // they inject Key_Event()s just like the SDL path above.
    IN_Evdev_Poll();
#endif
}

void IN_Init (void)
{
#ifdef __webos__
    IN_Evdev_Init();
#endif
    if ( COM_CheckParm ("-nomouse") )
        return;
    mouse_x = mouse_y = 0.0;
    joy_x = joy_y = 0.0;
    mouse_avail = 1;
}

void IN_Shutdown (void)
{
#ifdef __webos__
    IN_Evdev_Shutdown();
#endif
    mouse_avail = 0;
}

void IN_Commands (void)
{
    int i;
    int mouse_buttonstate;

    if (!mouse_avail) return;

    i = SDL_GetMouseState(NULL, NULL);
    /* Quake swaps the second and third buttons */
    mouse_buttonstate = (i & ~0x06) | ((i & 0x02)<<1) | ((i & 0x04)>>1);
    mousedown = mouse_buttonstate & 1;
    if ( !mousedown && autofire )
    {
        autofire = false;
    }

    Key_Event( K_MOUSE1, autofire );

}

void IN_Move (usercmd_t *cmd)
{
#ifdef __webos__
    // Analog controller movement/look (no-op if only a keyboard is attached).
    IN_Evdev_Move(cmd);
#endif

    if (!mouse_avail)
        return;

    mouse_x = joy_x * sensitivity.value * 2.5;
    mouse_y = joy_y * sensitivity.value * 2.5;

    //if ( (in_strafe.state & 1) || (lookstrafe.value && (in_mlook.state & 1) ))
    if( gesturedown )
        cmd->sidemove += m_side.value * mouse_x;
    else
        cl.viewangles[YAW] -= m_yaw.value * mouse_x;
    if (in_mlook.state & 1)
        V_StopPitchDrift ();
   
    cmd->forwardmove -= m_forward.value * mouse_y;
    //if ( (in_mlook.state & 1) && !(in_strafe.state & 1)) {
    //    cl.viewangles[PITCH] += m_pitch.value * mouse_y;
    //    if (cl.viewangles[PITCH] > 80)
    //        cl.viewangles[PITCH] = 80;
    //    if (cl.viewangles[PITCH] < -70)
    //        cl.viewangles[PITCH] = -70;
    //} else {
    //    if ((in_strafe.state & 1) && noclip_anglehack)
    //        cmd->upmove -= m_forward.value * mouse_y;
    //    else
    //        cmd->forwardmove -= m_forward.value * mouse_y;
    //}
    mouse_x = mouse_y = 0.0;

    if ( !mousedown )
    {
        joy_x = joy_y = 0.0;
    }
}

/*
================
Sys_ConsoleInput
================
*/
char *Sys_ConsoleInput (void)
{
    return 0;
}
