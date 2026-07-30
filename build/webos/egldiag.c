// egldiag.c -- interrogate the TouchPad's EGL directly: which step fails, with
// what error code, and what configs the driver actually offers.
//
// DIAGNOSTIC ONLY. Shipping code must let SDL own the GL context (direct EGL
// breaks the 3-layer compositor -- touch flicker). But SDL's single "Could not
// create EGL context" line hides everything; this prints the whole story.
//
// Everything is resolved via dlopen/dlsym from the device's own /usr/lib
// libraries, so we need no EGL headers or link libs from the PDK.
#include <stdio.h>
#include <stdlib.h>
#include <unistd.h>
#include <dlfcn.h>

typedef void        *EGLDisplay;
typedef void        *EGLConfig;
typedef void        *EGLContext;
typedef void        *EGLSurface;
typedef int          EGLint;
typedef unsigned int EGLBoolean;
typedef void        *NativeDisplayType;

#define EGL_DEFAULT_DISPLAY  ((NativeDisplayType)0)
#define EGL_NO_CONTEXT       ((EGLContext)0)
#define EGL_NO_SURFACE       ((EGLSurface)0)
#define EGL_NONE             0x3038
#define EGL_VENDOR           0x3053
#define EGL_VERSION          0x3054
#define EGL_EXTENSIONS       0x3055
#define EGL_CLIENT_APIS      0x308D
#define EGL_BUFFER_SIZE      0x3020
#define EGL_ALPHA_SIZE       0x3021
#define EGL_BLUE_SIZE        0x3022
#define EGL_GREEN_SIZE       0x3023
#define EGL_RED_SIZE         0x3024
#define EGL_DEPTH_SIZE       0x3025
#define EGL_STENCIL_SIZE     0x3026
#define EGL_SURFACE_TYPE     0x3033
#define EGL_CONFIG_ID        0x3028
#define EGL_RENDERABLE_TYPE  0x3040
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_OPENGL_ES_BIT    0x0001
#define EGL_OPENGL_ES2_BIT   0x0004
#define EGL_WINDOW_BIT       0x0004
#define EGL_PBUFFER_BIT      0x0001
#define EGL_WIDTH            0x3057
#define EGL_HEIGHT           0x3056

static EGLint      (*p_eglGetError)(void);
static EGLDisplay  (*p_eglGetDisplay)(NativeDisplayType);
static EGLBoolean  (*p_eglInitialize)(EGLDisplay, EGLint *, EGLint *);
static const char *(*p_eglQueryString)(EGLDisplay, EGLint);
static EGLBoolean  (*p_eglGetConfigs)(EGLDisplay, EGLConfig *, EGLint, EGLint *);
static EGLBoolean  (*p_eglGetConfigAttrib)(EGLDisplay, EGLConfig, EGLint, EGLint *);
static EGLContext  (*p_eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
static EGLBoolean  (*p_eglDestroyContext)(EGLDisplay, EGLContext);
static EGLSurface  (*p_eglCreatePbufferSurface)(EGLDisplay, EGLConfig, const EGLint *);
static EGLBoolean  (*p_eglDestroySurface)(EGLDisplay, EGLSurface);
static EGLBoolean  (*p_eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
static EGLBoolean  (*p_eglBindAPI)(unsigned int);

static const char *errname(EGLint e)
{
    switch (e) {
        case 0x3000: return "EGL_SUCCESS";
        case 0x3001: return "EGL_NOT_INITIALIZED";
        case 0x3002: return "EGL_BAD_ACCESS";
        case 0x3003: return "EGL_BAD_ALLOC";
        case 0x3004: return "EGL_BAD_ATTRIBUTE";
        case 0x3005: return "EGL_BAD_CONFIG";
        case 0x3006: return "EGL_BAD_CONTEXT";
        case 0x3007: return "EGL_BAD_CURRENT_SURFACE";
        case 0x3008: return "EGL_BAD_DISPLAY";
        case 0x3009: return "EGL_BAD_MATCH";
        case 0x300A: return "EGL_BAD_NATIVE_PIXMAP";
        case 0x300B: return "EGL_BAD_NATIVE_WINDOW";
        case 0x300C: return "EGL_BAD_PARAMETER";
        case 0x300D: return "EGL_BAD_SURFACE";
        case 0x300E: return "EGL_CONTEXT_LOST";
        default:     return "?";
    }
}

#define RESOLVE(h, n) do { \
        p_##n = dlsym(h, #n); \
        if (!p_##n) { printf("FATAL: dlsym(%s) failed: %s\n", #n, dlerror()); return 1; } \
    } while (0)

int main(void)
{
    void *h;
    EGLDisplay dpy;
    EGLint maj = 0, min = 0, ncfg = 0, i;
    EGLConfig cfgs[64];

    // Log to the same place the smoke test used, appending.
    {
        FILE *lf = fopen("/media/internal/quakehd-gl.log", "a");
        if (lf) { dup2(fileno(lf), 1); dup2(fileno(lf), 2); fclose(lf); }
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    printf("\n===== egldiag: pid %d =====\n", (int)getpid());

    h = dlopen("/usr/lib/libEGL.so", RTLD_NOW | RTLD_GLOBAL);
    printf("dlopen(libEGL.so)          : %s\n", h ? "OK" : dlerror());
    if (!h) return 1;
    // The GLES client libraries must also be resident for context creation.
    printf("dlopen(libGLES_CM.so)      : %s\n",
           dlopen("/usr/lib/libGLES_CM.so", RTLD_NOW | RTLD_GLOBAL) ? "OK" : dlerror());

    RESOLVE(h, eglGetError);
    RESOLVE(h, eglGetDisplay);
    RESOLVE(h, eglInitialize);
    RESOLVE(h, eglQueryString);
    RESOLVE(h, eglGetConfigs);
    RESOLVE(h, eglGetConfigAttrib);
    RESOLVE(h, eglCreateContext);
    RESOLVE(h, eglDestroyContext);
    RESOLVE(h, eglCreatePbufferSurface);
    RESOLVE(h, eglDestroySurface);
    RESOLVE(h, eglMakeCurrent);
    p_eglBindAPI = dlsym(h, "eglBindAPI");   /* optional, EGL 1.2+ */

    dpy = p_eglGetDisplay(EGL_DEFAULT_DISPLAY);
    printf("eglGetDisplay(DEFAULT)     : %p  err=%s\n", dpy, errname(p_eglGetError()));
    if (!dpy) return 1;

    if (!p_eglInitialize(dpy, &maj, &min)) {
        printf("eglInitialize              : FAILED  err=%s\n", errname(p_eglGetError()));
        return 1;
    }
    printf("eglInitialize              : OK, EGL %d.%d\n", maj, min);
    printf("  VENDOR     : %s\n", p_eglQueryString(dpy, EGL_VENDOR));
    printf("  VERSION    : %s\n", p_eglQueryString(dpy, EGL_VERSION));
    printf("  CLIENT_APIS: %s\n", p_eglQueryString(dpy, EGL_CLIENT_APIS));
    printf("  EXTENSIONS : %s\n", p_eglQueryString(dpy, EGL_EXTENSIONS));

    if (!p_eglGetConfigs(dpy, cfgs, 64, &ncfg)) {
        printf("eglGetConfigs              : FAILED  err=%s\n", errname(p_eglGetError()));
        return 1;
    }
    printf("eglGetConfigs              : %d configs\n", ncfg);
    printf("  id  rgba      depth stencil surf  renderable\n");
    for (i = 0; i < ncfg && i < 64; i++) {
        EGLint id, r, g, b, a, d, s, st, rt;
        p_eglGetConfigAttrib(dpy, cfgs[i], EGL_CONFIG_ID, &id);
        p_eglGetConfigAttrib(dpy, cfgs[i], EGL_RED_SIZE, &r);
        p_eglGetConfigAttrib(dpy, cfgs[i], EGL_GREEN_SIZE, &g);
        p_eglGetConfigAttrib(dpy, cfgs[i], EGL_BLUE_SIZE, &b);
        p_eglGetConfigAttrib(dpy, cfgs[i], EGL_ALPHA_SIZE, &a);
        p_eglGetConfigAttrib(dpy, cfgs[i], EGL_DEPTH_SIZE, &d);
        p_eglGetConfigAttrib(dpy, cfgs[i], EGL_STENCIL_SIZE, &s);
        p_eglGetConfigAttrib(dpy, cfgs[i], EGL_SURFACE_TYPE, &st);
        p_eglGetConfigAttrib(dpy, cfgs[i], EGL_RENDERABLE_TYPE, &rt);
        printf("  %-3d %d%d%d%d      %-5d %-7d 0x%03x 0x%02x%s%s\n",
               id, r, g, b, a, d, s, st, rt,
               (rt & EGL_OPENGL_ES_BIT)  ? " ES1" : "",
               (rt & EGL_OPENGL_ES2_BIT) ? " ES2" : "");
    }

    // Try to create a context against every config, ES1 and ES2 flavours.
    printf("context creation attempts:\n");
    for (i = 0; i < ncfg && i < 64; i++) {
        static const EGLint es1_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 1, EGL_NONE };
        static const EGLint es2_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
        EGLint id;
        EGLContext c;

        p_eglGetConfigAttrib(dpy, cfgs[i], EGL_CONFIG_ID, &id);

        c = p_eglCreateContext(dpy, cfgs[i], EGL_NO_CONTEXT, NULL);
        printf("  cfg %-3d  noattr: %s (%s)", id, c ? "OK" : "fail",
               errname(p_eglGetError()));
        if (c) {
            // Prove it is usable: bind to a small pbuffer if the config allows.
            static const EGLint pb[] = { EGL_WIDTH, 64, EGL_HEIGHT, 64, EGL_NONE };
            EGLSurface s2 = p_eglCreatePbufferSurface(dpy, cfgs[i], pb);
            if (s2) {
                EGLBoolean mc = p_eglMakeCurrent(dpy, s2, s2, c);
                printf("  pbuffer+makeCurrent: %s (%s)",
                       mc ? "OK" : "fail", errname(p_eglGetError()));
                p_eglMakeCurrent(dpy, EGL_NO_SURFACE, EGL_NO_SURFACE, EGL_NO_CONTEXT);
                p_eglDestroySurface(dpy, s2);
            } else {
                printf("  pbuffer: fail (%s)", errname(p_eglGetError()));
            }
            p_eglDestroyContext(dpy, c);
        }

        c = p_eglCreateContext(dpy, cfgs[i], EGL_NO_CONTEXT, es1_attr);
        printf("  es1: %s (%s)", c ? "OK" : "fail", errname(p_eglGetError()));
        if (c) p_eglDestroyContext(dpy, c);

        c = p_eglCreateContext(dpy, cfgs[i], EGL_NO_CONTEXT, es2_attr);
        printf("  es2: %s (%s)\n", c ? "OK" : "fail", errname(p_eglGetError()));
        if (c) p_eglDestroyContext(dpy, c);
    }

    printf("egldiag DONE\n");
    return 0;
}
