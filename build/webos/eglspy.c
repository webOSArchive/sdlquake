// eglspy.c -- PASSIVE EGL call logger (LD_PRELOAD). Observes only: every call
// forwards to the real /usr/lib/libEGL.so unmodified. Used to capture the EGL
// sequence a KNOWN-WORKING GL app (Tux Racer) performs, to compare against our
// own failing one.
//
// Palm's SDL 1.2 dlopens the library named by $SDL_VIDEO_EGL_DRIVER (default
// libEGL.so) and dlsyms every egl* entry point from it. Raw EGL works from a
// shell (see egldiag.c) but SDL's GL init fails at "Could not create EGL
// context" -- so point SDL at THIS library instead: every call forwards to the
// real /usr/lib/libEGL.so and logs its arguments and result, revealing exactly
// what SDL does differently from our working sequence.
//
//   SDL_VIDEO_EGL_DRIVER=/media/internal/eglspy.so ./glsmoke
#include <stdio.h>
#include <stdarg.h>
#include <dlfcn.h>
#include <unistd.h>

typedef void        *EGLDisplay;
typedef void        *EGLConfig;
typedef void        *EGLContext;
typedef void        *EGLSurface;
typedef int          EGLint;
typedef unsigned int EGLBoolean;
typedef void        *NativeDisplayType;
typedef void        *NativeWindowType;
typedef void        *NativePixmapType;

static void *real;
static void logf_(const char *fmt, ...)
{
    FILE *f = fopen("/media/internal/eglspy.log", "a");
    va_list ap;
    if (!f) return;
    va_start(ap, fmt);
    vfprintf(f, fmt, ap);
    va_end(ap);
    fputc('\n', f);
    fclose(f);
}

static void *fn(const char *name)
{
    if (!real) {
        real = dlopen("/usr/lib/libEGL.so", RTLD_NOW | RTLD_GLOBAL);
        logf_("eglspy: loaded real libEGL -> %p", real);
    }
    return real ? dlsym(real, name) : 0;
}

static const char *errname(EGLint e)
{
    static const char *n[] = { "SUCCESS", "NOT_INITIALIZED", "BAD_ACCESS",
        "BAD_ALLOC", "BAD_ATTRIBUTE", "BAD_CONFIG", "BAD_CONTEXT",
        "BAD_CURRENT_SURFACE", "BAD_DISPLAY", "BAD_MATCH", "BAD_NATIVE_PIXMAP",
        "BAD_NATIVE_WINDOW", "BAD_PARAMETER", "BAD_SURFACE", "CONTEXT_LOST" };
    if (e >= 0x3000 && e <= 0x300E) return n[e - 0x3000];
    return "?";
}

static EGLint geterr(void)
{
    static EGLint (*p)(void);
    if (!p) p = fn("eglGetError");
    return p ? p() : -1;
}

static void log_attribs(const char *tag, const EGLint *a)
{
    char buf[512];
    int n = 0;
    buf[0] = 0;
    if (!a) { logf_("%s attribs=NULL", tag); return; }
    while (*a != 0x3038 && n < 400) {          /* EGL_NONE */
        n += snprintf(buf + n, sizeof(buf) - n, " 0x%04x=%d", a[0], a[1]);
        a += 2;
    }
    logf_("%s attribs:%s", tag, buf[0] ? buf : " (empty)");
}

EGLint eglGetError(void) { return geterr(); }

EGLDisplay eglGetDisplay(NativeDisplayType d)
{
    static EGLDisplay (*p)(NativeDisplayType);
    EGLDisplay r;
    if (!p) p = fn("eglGetDisplay");
    r = p(d);
    logf_("eglGetDisplay(native=%p) -> %p", d, r);
    return r;
}

EGLBoolean eglInitialize(EGLDisplay d, EGLint *maj, EGLint *min)
{
    static EGLBoolean (*p)(EGLDisplay, EGLint *, EGLint *);
    EGLBoolean r;
    if (!p) p = fn("eglInitialize");
    r = p(d, maj, min);
    logf_("eglInitialize(%p) -> %d (%s) v=%d.%d", d, r, errname(geterr()),
          maj ? *maj : -1, min ? *min : -1);
    return r;
}

EGLBoolean eglTerminate(EGLDisplay d)
{
    static EGLBoolean (*p)(EGLDisplay);
    if (!p) p = fn("eglTerminate");
    logf_("eglTerminate(%p)", d);
    return p(d);
}

const char *eglQueryString(EGLDisplay d, EGLint name)
{
    static const char *(*p)(EGLDisplay, EGLint);
    if (!p) p = fn("eglQueryString");
    return p(d, name);
}

void (*eglGetProcAddress(const char *name))(void)
{
    static void (*(*p)(const char *))(void);
    if (!p) p = fn("eglGetProcAddress");
    logf_("eglGetProcAddress(%s)", name);
    return p(name);
}

EGLBoolean eglGetConfigs(EGLDisplay d, EGLConfig *c, EGLint sz, EGLint *n)
{
    static EGLBoolean (*p)(EGLDisplay, EGLConfig *, EGLint, EGLint *);
    EGLBoolean r;
    if (!p) p = fn("eglGetConfigs");
    r = p(d, c, sz, n);
    logf_("eglGetConfigs -> %d, n=%d", r, n ? *n : -1);
    return r;
}

EGLBoolean eglChooseConfig(EGLDisplay d, const EGLint *attr, EGLConfig *c,
                           EGLint sz, EGLint *n)
{
    static EGLBoolean (*p)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *);
    EGLBoolean r;
    if (!p) p = fn("eglChooseConfig");
    log_attribs("eglChooseConfig", attr);
    r = p(d, attr, c, sz, n);
    logf_("eglChooseConfig -> %d (%s), n=%d, cfg[0]=%p", r, errname(geterr()),
          n ? *n : -1, (c && n && *n) ? c[0] : 0);
    return r;
}

EGLBoolean eglGetConfigAttrib(EGLDisplay d, EGLConfig c, EGLint a, EGLint *v)
{
    static EGLBoolean (*p)(EGLDisplay, EGLConfig, EGLint, EGLint *);
    if (!p) p = fn("eglGetConfigAttrib");
    return p(d, c, a, v);
}

EGLSurface eglCreateWindowSurface(EGLDisplay d, EGLConfig c,
                                  NativeWindowType w, const EGLint *attr)
{
    static EGLSurface (*p)(EGLDisplay, EGLConfig, NativeWindowType, const EGLint *);
    EGLSurface r;
    if (!p) p = fn("eglCreateWindowSurface");
    log_attribs("eglCreateWindowSurface", attr);
    r = p(d, c, w, attr);
    logf_("eglCreateWindowSurface(dpy=%p cfg=%p win=%p) -> %p (%s)",
          d, c, w, r, errname(geterr()));
    if (r) {   /* the surface's true pixel size -- SDL's struct can lie */
        static EGLBoolean (*q)(EGLDisplay, EGLSurface, EGLint, EGLint *);
        EGLint sw = -1, sh = -1;
        if (!q) q = fn("eglQuerySurface");
        if (q) {
            q(d, r, 0x3057, &sw);   /* EGL_WIDTH  */
            q(d, r, 0x3056, &sh);   /* EGL_HEIGHT */
            logf_("eglQuerySurface: ACTUAL surface %dx%d", sw, sh);
        }
    }
    return r;
}

EGLSurface eglCreatePbufferSurface(EGLDisplay d, EGLConfig c, const EGLint *attr)
{
    static EGLSurface (*p)(EGLDisplay, EGLConfig, const EGLint *);
    EGLSurface r;
    if (!p) p = fn("eglCreatePbufferSurface");
    r = p(d, c, attr);
    logf_("eglCreatePbufferSurface -> %p (%s)", r, errname(geterr()));
    return r;
}

EGLBoolean eglDestroySurface(EGLDisplay d, EGLSurface s)
{
    static EGLBoolean (*p)(EGLDisplay, EGLSurface);
    if (!p) p = fn("eglDestroySurface");
    logf_("eglDestroySurface(%p)", s);
    return p(d, s);
}

EGLContext eglCreateContext(EGLDisplay d, EGLConfig c, EGLContext share,
                            const EGLint *attr)
{
    static EGLContext (*p)(EGLDisplay, EGLConfig, EGLContext, const EGLint *);
    EGLContext r;
    if (!p) p = fn("eglCreateContext");
    log_attribs("eglCreateContext", attr);
    r = p(d, c, share, attr);
    logf_("eglCreateContext(dpy=%p cfg=%p share=%p) -> %p (%s)",
          d, c, share, r, errname(geterr()));
    return r;
}

EGLBoolean eglDestroyContext(EGLDisplay d, EGLContext c)
{
    static EGLBoolean (*p)(EGLDisplay, EGLContext);
    if (!p) p = fn("eglDestroyContext");
    logf_("eglDestroyContext(%p)", c);
    return p(d, c);
}

EGLBoolean eglMakeCurrent(EGLDisplay d, EGLSurface dr, EGLSurface rd, EGLContext c)
{
    static EGLBoolean (*p)(EGLDisplay, EGLSurface, EGLSurface, EGLContext);
    EGLBoolean r;
    if (!p) p = fn("eglMakeCurrent");
    r = p(d, dr, rd, c);
    logf_("eglMakeCurrent(draw=%p read=%p ctx=%p) -> %d (%s)",
          dr, rd, c, r, errname(geterr()));
    return r;
}

EGLBoolean eglSwapBuffers(EGLDisplay d, EGLSurface s)
{
    static EGLBoolean (*p)(EGLDisplay, EGLSurface);
    static int logged;
    EGLBoolean r;
    if (!p) p = fn("eglSwapBuffers");
    r = p(d, s);
    if (!logged) { logf_("eglSwapBuffers(first) -> %d (%s)", r, errname(geterr())); logged = 1; }
    return r;
}

EGLBoolean eglSwapInterval(EGLDisplay d, EGLint i)
{
    static EGLBoolean (*p)(EGLDisplay, EGLint);
    if (!p) p = fn("eglSwapInterval");
    logf_("eglSwapInterval(%d)", i);
    return p(d, i);
}

EGLBoolean eglBindAPI(unsigned int api)
{
    static EGLBoolean (*p)(unsigned int);
    if (!p) p = fn("eglBindAPI");
    logf_("eglBindAPI(0x%x)", api);
    return p ? p(api) : 0;
}

EGLBoolean eglWaitGL(void)
{
    static EGLBoolean (*p)(void);
    if (!p) p = fn("eglWaitGL");
    return p ? p() : 0;
}

EGLBoolean eglWaitNative(EGLint e)
{
    static EGLBoolean (*p)(EGLint);
    if (!p) p = fn("eglWaitNative");
    return p ? p(e) : 0;
}
