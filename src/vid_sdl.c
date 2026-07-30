// vid_sdl.h -- sdl video driver 

#include "SDL.h"
#include "quakedef.h"
#include "d_local.h"
#include "pdl.h"
#include "in_evdev.h"
// key_dest (used by the menu-aware touch overlay) comes from keys.h via quakedef.h

viddef_t    vid;                // global video state
unsigned short  d_8to16table[256];

// The original defaults
// Native TouchPad panel resolution. This tree targets the tablet only -- the
// phone build is a separate, long-stable release. (Was 480x320, the Palm Pre's
// screen, which the compositor then upscaled 2.1x.)
#define    BASEWIDTH    1024
#define    BASEHEIGHT   768

#define    FIRE_SIZE    160
#define    JUMP_SIZE    120
#define    JOY_SIZE     160
#define    JOY_DEAD     10
#define    JOY_X        80
#define    JOY_Y        ( (float)vid.height - 80.0f )

#define    JOY_IMAGE_FILENAME        "images/joystick.png"
#define    JOY_PRESS_IMAGE_FILENAME  "images/joystick-press.png"
#define    JUMP_IMAGE_FILENAME       "images/jump.png"
#define    FIRE_IMAGE_FILENAME       "images/fire.png"

#define    OVERLAY_ITEM_COUNT   4
#define    OVERLAY_ALPHA       32

byte    autofire = 0;
byte    mousedown = 0;
byte    normalkeyboard = 0;
byte    drawoverlay = 1;
byte    gesturedown = 0;
extern int in_impulse;

//Hacky way to show the 'jumping' overlay item
int     jumping_counter = 0;
#define JUMP_FRAME_COUNT 6

int     fire_counter = 0;
#define FIRE_FRAME_COUNT 1

int    VGA_width, VGA_height, VGA_rowbytes, VGA_bufferrowbytes = 0;
byte    *VGA_pagebase;

static SDL_Surface *buffer = NULL;
static SDL_Surface *screen = NULL;

static qboolean mouse_avail;
static float   mouse_x, mouse_y;
static float   joy_x, joy_y;

// No support for option menus
void (*vid_menudrawfn)(void) = NULL;
void (*vid_menukeyfn)(int key) = NULL;

// I don't have the header for this...
SDL_Surface * IMG_Load( char * filename );

static SDL_Surface * joy_img = NULL;
static SDL_Surface * joy_press_img = NULL;
static SDL_Surface * jump_img = NULL;
static SDL_Surface * fire_img = NULL;

static SDL_Rect old_rects[OVERLAY_ITEM_COUNT];


void    VID_SetPalette (unsigned char *palette)
{
    int i;
    SDL_Color colors[256];

    for ( i=0; i<256; ++i )
    {
        colors[i].r = *palette++;
        colors[i].g = *palette++;
        colors[i].b = *palette++;
    }
    SDL_SetColors(screen, colors, 0, 256);
    SDL_SetColors(buffer, colors, 0, 256);
}

void    VID_ShiftPalette (unsigned char *palette)
{
    VID_SetPalette(palette);
}

SDL_Surface * LoadImage( char * image )
{
    SDL_Surface * img = IMG_Load( image );
    if ( !img )
    {
        Sys_Error( "Error loading image %s: %s\n", image, SDL_GetError() );
    }

    SDL_SetAlpha( img, SDL_SRCALPHA, OVERLAY_ALPHA );
    return img;
}

void    VID_Init (unsigned char *palette)
{
    int pnum, chunk;
    byte *cache;
    int cachesize;
    Uint8 video_bpp;
    Uint16 video_w, video_h;
    Uint32 flags;

    // Load the SDL library
    if (SDL_Init(SDL_INIT_VIDEO|SDL_INIT_AUDIO) < 0)
        Sys_Error("VID: Couldn't load SDL: %s", SDL_GetError());

    // Set up display mode (width and height)
    vid.width = BASEWIDTH;
    vid.height = BASEHEIGHT;
    vid.maxwarpwidth = WARP_WIDTH;
    vid.maxwarpheight = WARP_HEIGHT;
    if ((pnum=COM_CheckParm("-winsize")))
    {
        if (pnum >= com_argc-2)
            Sys_Error("VID: -winsize <width> <height>\n");
        vid.width = Q_atoi(com_argv[pnum+1]);
        vid.height = Q_atoi(com_argv[pnum+2]);
        if (!vid.width || !vid.height)
            Sys_Error("VID: Bad window width/height\n");
    }

    // Set video width, height and flags
    flags = (SDL_SWSURFACE|SDL_HWPALETTE);
    if ( COM_CheckParm ("-fullscreen") )
        flags |= SDL_FULLSCREEN;
#ifdef __webos__
    // Always fullscreen on the TouchPad: the compositor scales our 480x320
    // buffer to the panel, and a fullscreen app keeps the backlight on -- which
    // is what stops a Bluetooth gamepad session dying to display-sleep churn
    // (see the hardware-accessories knowledge doc).
    flags |= SDL_FULLSCREEN;
#endif


    // Initialize display 
    if (!(screen = SDL_SetVideoMode(vid.width, vid.height, 32, flags)))
        Sys_Error("VID: Couldn't set video mode: %s\n", SDL_GetError());

    //Lock the display to landscape. (Was a bug: the "!= PDL_NOERROR" was inside
    //the call, so it passed a bool as the orientation and errored on success.)
    if ( PDL_SetOrientation( PDL_ORIENTATION_LEFT ) != PDL_NOERROR )
        Con_Printf( "PDL: failed to set orientation\n" );
    PDL_CustomPauseUiEnable( PDL_FALSE );

    //create buffer where we do the rendering
    buffer = SDL_CreateRGBSurface(SDL_SWSURFACE,
            screen->w,
            screen->h,
            8, //screen->format->BitsPerPixel,
            0,
            0,
            0,
            0);
    if ( !buffer )
    {
        Sys_Error( "VID: Couldn't create buffer: %s\n", SDL_GetError() );
    }
    VID_SetPalette(palette);
    SDL_WM_SetCaption("sdlquake","sdlquake");
    // now know everything we need to know about the buffer
    VGA_width = vid.conwidth = vid.width;
    VGA_height = vid.conheight = vid.height;
    vid.aspect = ((float)vid.height / (float)vid.width) * (320.0 / 240.0);
    vid.numpages = 1;
    vid.colormap = host_colormap;
    vid.fullbright = 256 - LittleLong (*((int *)vid.colormap + 2048));
    VGA_pagebase = vid.buffer = buffer->pixels;
    VGA_rowbytes = vid.rowbytes = buffer->pitch;
    vid.conbuffer = vid.buffer;
    vid.conrowbytes = vid.rowbytes;
    vid.direct = 0;
    
    // allocate z buffer and surface cache
    chunk = vid.width * vid.height * sizeof (*d_pzbuffer);
    cachesize = D_SurfaceCacheForRes (vid.width, vid.height);
    chunk += cachesize;
    d_pzbuffer = Hunk_HighAllocName(chunk, "video");
    if (d_pzbuffer == NULL)
        Sys_Error ("Not enough memory for video mode\n");

    // initialize the cache memory 
        cache = (byte *) d_pzbuffer
                + vid.width * vid.height * sizeof (*d_pzbuffer);
    D_InitCaches (cache, cachesize);

    // initialize the mouse
    SDL_ShowCursor(0);

    // Load the overlay images
    joy_img = LoadImage( JOY_IMAGE_FILENAME );
    joy_press_img = LoadImage( JOY_PRESS_IMAGE_FILENAME );
    jump_img = LoadImage( JUMP_IMAGE_FILENAME );
    fire_img = LoadImage( FIRE_IMAGE_FILENAME );

    memset( old_rects, 0, sizeof( old_rects ) );
}

void    VID_Shutdown (void)
{
    SDL_Quit();
}


/* ===========================================================================
 * D_DrawOverlayBacking
 *   
 *  Description:  Draws where overlay used to be (making sure it's gone) as well
 *                as where it will be in preparation for the overlay being drawn
 * =========================================================================*/
void D_DrawOverlayBacking( SDL_Rect rects[OVERLAY_ITEM_COUNT] )
{
    int i = 0;
    for ( ; i < OVERLAY_ITEM_COUNT; i++ )
    {
        SDL_Rect *r_old = &old_rects[i];
        SDL_Rect *r_new = &rects[i];

        SDL_BlitSurface( buffer, r_old, screen, r_old );
        SDL_BlitSurface( buffer, r_new, screen, r_new );
    }

    memcpy( old_rects, rects, sizeof( old_rects ) );
}


void D_DrawUIOverlay()
{
    // Render the overlay!
    // This is done in two stages:
    // Calculate the rects where everything goes,
    // and then the actual drawing
    SDL_Rect rect[OVERLAY_ITEM_COUNT];
    SDL_Rect emptyrect;
    int i;
    memset( rect, 0, sizeof( rect ) );


    //always draw the joystick location indicator
    rect[0].x = JOY_X - joy_img->w/2;
    rect[0].y = JOY_Y - joy_img->h/2;
    rect[0].w = joy_img->w;
    rect[0].h = joy_img->h;


    //If user has moved the joystick...
    rect[1].x = JOY_X + joy_x - joy_press_img->w/2;
    rect[1].y = JOY_Y + joy_y - joy_press_img->h/2;
    rect[1].w = joy_press_img->w;
    rect[1].h = joy_press_img->h;

    //Adjust x/y so the full image is on the screen
    int max_x = vid.width - joy_press_img->w -1;
    int max_y = vid.height - joy_press_img->h -1;

    if ( rect[1].x > max_x )
    {
        rect[1].x = max_x;
    }
    if ( rect[1].y > max_y )
    {
        rect[1].y = max_y;
    }


    //Draw the jump button if the user hit it
    rect[2].x = vid.width/2 - jump_img->w/2;
    rect[2].y = 10;
    rect[2].w = jump_img->w;
    rect[2].h = jump_img->h;

    //Draw the fire button if the user is hitting it
    rect[3].x = vid.width - FIRE_SIZE/2 - fire_img->w/2;
    rect[3].y = vid.height - FIRE_SIZE/2 - fire_img->h/2;
    rect[3].w = fire_img->w;
    rect[3].h = fire_img->h;

    //Now render the overlay

    //First make sure we a)get rid of the old overlay on the screen buffer
    //and b)the pieces we're drawing on top of are up-to-date
    D_DrawOverlayBacking( rect );

    if ( !drawoverlay )
    {
        return;
    }

    SDL_BlitSurface( joy_img, NULL, screen, &rect[0] );

    SDL_BlitSurface( joy_press_img, NULL, screen, &rect[1] );

    if ( jumping_counter > 0 )
    {
        jumping_counter--;
        SDL_BlitSurface( jump_img, NULL, screen, &rect[2] );
    }

    SDL_BlitSurface( fire_img, NULL, screen, &rect[3] );

}

void    VID_Update (vrect_t *rects)
{
    SDL_Rect *sdlrects;
    int n, i;
    vrect_t *rect;

    // Two-pass system, since Quake doesn't do it the SDL way...

    // First, count the number of rectangles
    n = 0;
    for (rect = rects; rect; rect = rect->pnext)
        ++n;

    // Second, copy them to SDL rectangles and update
    if (!(sdlrects = (SDL_Rect *)alloca(n*sizeof(*sdlrects))))
        Sys_Error("Out of memory");
    i = 0;
    for (rect = rects; rect; rect = rect->pnext)
    {
        sdlrects[i].x = rect->x;
        sdlrects[i].y = rect->y;
        sdlrects[i].w = rect->width;
        sdlrects[i].h = rect->height;
        SDL_BlitSurface( buffer, &sdlrects[i], screen, &sdlrects[i] );
        ++i;
    }

    D_DrawUIOverlay();
    SDL_UpdateRect( screen, 0, 0, 0, 0 );
}

/*
================
D_BeginDirectRect
================
*/
void D_BeginDirectRect (int x, int y, byte *pbitmap, int width, int height)
{
    Uint8 *offset;


    if (!screen) return;
    if ( x < 0 ) x = buffer->w+x-1;
    offset = (Uint8 *)buffer->pixels + y*buffer->pitch + x;
    while ( height-- )
    {
        memcpy(offset, pbitmap, width);
        offset += screen->pitch;
        pbitmap += width;
    }

}


/*
================
D_EndDirectRect
================
*/
void D_EndDirectRect (int x, int y, int width, int height)
{
    SDL_Rect update;
    if (!screen) return;
    if (x < 0) x = screen->w+x-1;

    update.x = x;
    update.y = y;
    update.w = width;
    update.h = height;
    SDL_BlitSurface( buffer, &update, screen, &update );
    D_DrawUIOverlay();
    SDL_UpdateRect( screen, 0, 0, 0, 0 );
}


/* ===========================================================================
 * Menu-aware touch overlay
 *   Quake menus only act on K_ENTER / K_ESCAPE / arrow keys. The on-screen
 *   overlay normally sends K_MOUSE1 (fire) + analog joystick, none of which a
 *   menu understands -- so a touch-only player could reach the menu but never
 *   SELECT anything. When key_dest isn't key_game we reinterpret the overlay:
 *     FIRE button  -> Enter (select)
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
