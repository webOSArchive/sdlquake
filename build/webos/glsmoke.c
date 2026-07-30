// glsmoke.c -- prove SDL + OpenGL ES 1.1 works on the TouchPad before porting
// GLQuake to it, and report what the Adreno driver actually gives us.
//
// Deliberately mirrors the constraints the real port must live under (see
// CONTROLLERS.md / the webOS PDK knowledge doc):
//   * SDL owns the GL context -- SDL_SetVideoMode(SDL_OPENGL) and
//     SDL_GL_SwapBuffers(). Calling EGL directly makes the TouchPad's 3-layer
//     compositor flicker on every touch.
//   * PDL_Init() before SDL_Init().
//   * GLES 1.1 has no immediate mode, so even this triangle uses vertex arrays.
//
// Build with the PDK's own gcc (GLIBC_2.4); prints to stdout, so redirect it.
#include <stdio.h>
#include <string.h>
#include <stdlib.h>
#include <unistd.h>
#include <SDL.h>
#include <GLES/gl.h>
#include <PDL.h>

#define W 1024
#define H 768
#define LOGPATH "/media/internal/quakehd-gl.log"

static void report_attr(const char *name, SDL_GLattr a)
{
    int v = -1;
    SDL_GL_GetAttribute(a, &v);
    printf("  %-22s %d\n", name, v);
}

int main(int argc, char **argv)
{
    SDL_Surface *screen;
    const char *s;
    int frames = 0, secs = 8;
    Uint32 t0, now, last_report;

    // A launcher-launched PDK app's stdout/stderr reach neither a terminal nor
    // /var/log/messages -- they are simply lost. So redirect them to writable
    // storage FIRST, before anything that can fail, or this test reports
    // nothing at all. /media/internal is bind-mounted rw into the jail.
    {
        FILE *lf = fopen(LOGPATH, "a");
        if (lf) {
            dup2(fileno(lf), 1);
            dup2(fileno(lf), 2);
            fclose(lf);
        }
    }
    setvbuf(stdout, NULL, _IONBF, 0);
    setvbuf(stderr, NULL, _IONBF, 0);

    /* Optional EGL spy injection that keeps /proc/self/exe (and therefore the
     * LS2 role match) intact: re-exec ourselves once with LD_PRELOAD set. A
     * separate wrapper binary would change the exe path and break the role. */
    if (!getenv("GLSMOKE_RESPAWNED") &&
        access("/media/internal/eglspy.so", 0) == 0) {
        char self[512];
        int n = readlink("/proc/self/exe", self, sizeof(self) - 1);
        if (n > 0) {
            self[n] = 0;
            setenv("LD_PRELOAD", "/media/internal/eglspy.so", 1);
            setenv("GLSMOKE_RESPAWNED", "1", 1);
            printf("respawning with eglspy: %s\n", self);
            execv(self, argv);
            printf("respawn failed, continuing without spy\n");
        }
    }

    printf("\n===== glsmoke run: pid %d =====\n", (int)getpid());
    {
        int i;
        for (i = 0; i < argc; i++) printf("  argv[%d] = %.60s\n", i, argv[i]);
        /* LunaSysMgr passes the launch-params JSON as argv[1]; only accept a
         * duration that is actually a number, or atoi("{...}")=0 makes the
         * app exit after one frame -- indistinguishable from a crash. */
        if (argc > 1 && argv[1][0] >= '0' && argv[1][0] <= '9')
            secs = atoi(argv[1]);
    }

    if (PDL_Init(0) != PDL_NOERROR)
        printf("PDL_Init failed (continuing)\n");

    PDL_SetOrientation(PDL_ORIENTATION_270);

    if (SDL_Init(SDL_INIT_VIDEO) < 0) {
        printf("FAIL: SDL_Init: %s\n", SDL_GetError());
        return 1;
    }

    // Try a matrix of configs. If EVERY one fails identically, the problem is
    // not the pixel format -- it is that a GL surface needs the app to be
    // launched by LunaSysMgr, not run from a shell.
    {
        static const struct {
            const char *name;
            Uint32 glflag;      /* SDL_OPENGL vs SDL_OPENGLES */
            int  setver;        /* set SDL_GL_CONTEXT_MAJOR_VERSION? */
            int  r, g, b, depth;
        } cfg[] = {
            /* Tux Racer -- a known-working GLES app on this device -- produced
             * eglChooseConfig RGB 3/3/2 depth 16 with NO EGL_RENDERABLE_TYPE,
             * and eglCreateContext with NULL attribs. Ours asked for
             * RENDERABLE_TYPE=ES2_BIT and CLIENT_VERSION=2 and got BAD_ALLOC,
             * so the ES2 path is what fails. Find which knob selects ES1. */
            { "OPENGLES, no attrs",        SDL_OPENGLES, 0, -1,-1,-1, -1 },
            { "OPENGLES, 565 d16",         SDL_OPENGLES, 0,  5, 6, 5, 16 },
            { "OPENGLES, ver1, 565 d16",   SDL_OPENGLES, 1,  5, 6, 5, 16 },
            { "OPENGL,   ver1, 565 d16",   SDL_OPENGL,   1,  5, 6, 5, 16 },
            { "OPENGL,   no attrs",        SDL_OPENGL,   0, -1,-1,-1, -1 },
        };
        int i, n = (int)(sizeof(cfg) / sizeof(cfg[0]));
        screen = NULL;
        for (i = 0; i < n; i++) {
            if (cfg[i].r >= 0) {
                SDL_GL_SetAttribute(SDL_GL_RED_SIZE,   cfg[i].r);
                SDL_GL_SetAttribute(SDL_GL_GREEN_SIZE, cfg[i].g);
                SDL_GL_SetAttribute(SDL_GL_BLUE_SIZE,  cfg[i].b);
                SDL_GL_SetAttribute(SDL_GL_DEPTH_SIZE, cfg[i].depth);
                SDL_GL_SetAttribute(SDL_GL_DOUBLEBUFFER, 1);
            }
            if (cfg[i].setver) {
                SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 1);
                SDL_GL_SetAttribute(SDL_GL_CONTEXT_MINOR_VERSION, 1);
            }
            screen = SDL_SetVideoMode(W, H, 0, cfg[i].glflag | SDL_FULLSCREEN);
            printf("  %-26s %s\n", cfg[i].name,
                   screen ? "OK <=== usable" : SDL_GetError());
            if (screen) break;
        }
        if (!screen) {
            printf("FAIL: no GL config worked.\n");
            SDL_Quit();
            return 1;
        }
    }
    printf("OK: GL context %dx%d\n", screen->w, screen->h);
    printf("GL attributes actually granted:\n");
    report_attr("RED_SIZE",     SDL_GL_RED_SIZE);
    report_attr("GREEN_SIZE",   SDL_GL_GREEN_SIZE);
    report_attr("BLUE_SIZE",    SDL_GL_BLUE_SIZE);
    report_attr("ALPHA_SIZE",   SDL_GL_ALPHA_SIZE);
    report_attr("DEPTH_SIZE",   SDL_GL_DEPTH_SIZE);
    report_attr("DOUBLEBUFFER", SDL_GL_DOUBLEBUFFER);

    s = (const char *)glGetString(GL_VENDOR);     printf("GL_VENDOR   : %s\n", s ? s : "(null)");
    s = (const char *)glGetString(GL_RENDERER);   printf("GL_RENDERER : %s\n", s ? s : "(null)");
    s = (const char *)glGetString(GL_VERSION);    printf("GL_VERSION  : %s\n", s ? s : "(null)");
    s = (const char *)glGetString(GL_EXTENSIONS);
    printf("GL_EXTENSIONS:\n");
    if (s) {                                   /* one per line, it is a long list */
        char buf[4096]; char *p, *q;
        strncpy(buf, s, sizeof(buf) - 1); buf[sizeof(buf) - 1] = 0;
        for (p = buf; (q = strchr(p, ' ')) != NULL; p = q + 1) {
            *q = 0; if (*p) printf("  %s\n", p);
        }
        if (*p) printf("  %s\n", p);
    }

    // Limits that decide how the port must upload Quake's textures.
    {
        GLint v = 0;
        glGetIntegerv(GL_MAX_TEXTURE_SIZE, &v);   printf("MAX_TEXTURE_SIZE  : %d\n", v);
        glGetIntegerv(GL_MAX_TEXTURE_UNITS, &v);  printf("MAX_TEXTURE_UNITS : %d\n", v);
    }

    glViewport(0, 0, W, H);
    glClearColor(0.1f, 0.1f, 0.35f, 1.0f);
    glDisable(GL_DEPTH_TEST);
    glShadeModel(GL_SMOOTH);

    printf("-- rendering %d seconds; measuring swap rate --\n", secs);
    t0 = last_report = SDL_GetTicks();
    for (;;) {
        static const GLfloat verts[] = {
            -0.7f, -0.6f, 0.0f,   0.7f, -0.6f, 0.0f,   0.0f,  0.7f, 0.0f
        };
        static const GLubyte cols[] = {
            255,60,60,255,   60,255,60,255,   80,120,255,255
        };
        SDL_Event e;

        glClear(GL_COLOR_BUFFER_BIT);
        glEnableClientState(GL_VERTEX_ARRAY);
        glEnableClientState(GL_COLOR_ARRAY);
        glVertexPointer(3, GL_FLOAT, 0, verts);
        glColorPointer(4, GL_UNSIGNED_BYTE, 0, cols);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        glDisableClientState(GL_COLOR_ARRAY);
        glDisableClientState(GL_VERTEX_ARRAY);
        SDL_GL_SwapBuffers();
        frames++;

        while (SDL_PollEvent(&e)) if (e.type == SDL_QUIT) goto done;

        now = SDL_GetTicks();
        if (now - last_report >= 1000) {
            printf("  %d fps (clear + 1 tri at %dx%d)\n", frames, W, H);
            frames = 0; last_report = now;
        }
        if (now - t0 >= (Uint32)secs * 1000) break;
    }
done:

    {
        GLenum err = glGetError();
        printf("final glGetError: 0x%04x %s\n", err, err ? "(SOMETHING FAILED)" : "(clean)");
    }
    printf("DONE\n");
    SDL_Quit();
    return 0;
}
