// sdlspy.c -- PASSIVE LD_PRELOAD logger for SDL video setup.
//
// Observes only; every call forwards to the real libSDL unchanged. Used to see
// exactly what a known-working 1024x768 GLES app (Tux Racer) asks
// SDL_SetVideoMode for, and what surface it gets back, since our own request
// keeps coming back as a 320x480 (Palm Pre sized) surface.
#include <stdio.h>
#include <stdarg.h>
#include <dlfcn.h>
#include <unistd.h>

typedef struct SDL_Surface SDL_Surface;
typedef unsigned int Uint32;

static void *real;
static void lg(const char *fmt, ...)
{
    FILE *f = fopen("/media/internal/sdlspy.log", "a");
    va_list ap;
    if (!f) return;
    va_start(ap, fmt); vfprintf(f, fmt, ap); va_end(ap);
    fputc('\n', f); fclose(f);
}
static void *fn(const char *n)
{
    if (!real) real = dlopen("/usr/lib/libSDL-1.2.so.0", RTLD_NOW | RTLD_GLOBAL);
    return real ? dlsym(real, n) : 0;
}

// SDL_Surface's first fields are: Uint32 flags; SDL_PixelFormat *format;
// int w, h;  -- enough to read back the granted size without the real header.
struct surf_head { Uint32 flags; void *format; int w, h; };

SDL_Surface *SDL_SetVideoMode(int w, int h, int bpp, Uint32 flags)
{
    static SDL_Surface *(*p)(int, int, int, Uint32);
    SDL_Surface *s;
    if (!p) p = fn("SDL_SetVideoMode");
    lg("SDL_SetVideoMode(w=%d h=%d bpp=%d flags=0x%08x)%s%s%s",
       w, h, bpp, flags,
       (flags & 0x00000002) ? " OPENGL"   : "",
       (flags & 0x00000040) ? " OPENGLES" : "",
       (flags & 0x80000000) ? " FULLSCREEN" : "");
    s = p(w, h, bpp, flags);
    if (s) {
        struct surf_head *sh = (struct surf_head *)s;
        lg("  -> surface %dx%d flags=0x%08x", sh->w, sh->h, sh->flags);
    } else {
        lg("  -> NULL");
    }
    return s;
}

int SDL_GL_SetAttribute(int attr, int value)
{
    static int (*p)(int, int);
    if (!p) p = fn("SDL_GL_SetAttribute");
    lg("SDL_GL_SetAttribute(attr=%d, value=%d)", attr, value);
    return p(attr, value);
}
