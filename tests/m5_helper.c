/* M5: one OpenGL ES 2 triangle through Mesa EGL (surfaceless) + zink on
 * our PanVK ICD, read back with glReadPixels and checked like M2. No window system, no SDL.
 *
 * Loads libEGL.so and libGLESv2.so by dlopen (names from the B3.1 build; override with PB_EGL /
 * PB_GLES). zink finds the ICD through ZINK_VK_ICD (our zink_screen.c shim).
 *
 * Run (all in /data/local/tmp/pb):
 *   ZINK_VK_ICD=/data/local/tmp/pb/libvulkan_panfrost.so MESA_LOADER_DRIVER_OVERRIDE=zink \
 *   LD_LIBRARY_PATH=/data/local/tmp/pb ./linker64 /data/local/tmp/pb/m5h
 * Build: aarch64-linux-android29-clang -O1 m5_helper.c -o m5h -ldl
 * Exit 0 pass, 2 wrong pixels, 1 API failure.
 */
#include <dlfcn.h>
#include <stdio.h>
#include <stdint.h>
#include <stdlib.h>
#include <string.h>

#define W 256
#define H 256

/* Minimal EGL/GLES2 declarations, so no Mesa headers are needed at build time. */
typedef void *EGLDisplay, *EGLConfig, *EGLSurface, *EGLContext;
typedef int EGLint; typedef unsigned EGLBoolean, EGLenum;
typedef unsigned GLenum, GLuint, GLbitfield; typedef int GLint, GLsizei; typedef float GLfloat;
typedef unsigned char GLboolean; typedef char GLchar;
#define EGL_PLATFORM_SURFACELESS_MESA 0x31DD
#define EGL_NO_CONTEXT ((EGLContext)0)
#define EGL_OPENGL_ES_API 0x30A0
#define EGL_SURFACE_TYPE 0x3033
#define EGL_PBUFFER_BIT 0x0001
#define EGL_RENDERABLE_TYPE 0x3040
#define EGL_OPENGL_ES2_BIT 0x0004
#define EGL_RED_SIZE 0x3024
#define EGL_GREEN_SIZE 0x3023
#define EGL_BLUE_SIZE 0x3022
#define EGL_ALPHA_SIZE 0x3021
#define EGL_NONE 0x3038
#define EGL_WIDTH 0x3057
#define EGL_HEIGHT 0x3056
#define EGL_CONTEXT_CLIENT_VERSION 0x3098
#define EGL_VENDOR 0x3053
#define EGL_VERSION 0x3054
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_TRIANGLES 0x0004
#define GL_FLOAT 0x1406
#define GL_FALSE 0
#define GL_RGBA 0x1908
#define GL_UNSIGNED_BYTE 0x1401
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_COMPILE_STATUS 0x8B81
#define GL_LINK_STATUS 0x8B82
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02

static void die(const char *m) { fprintf(stderr, "FAIL: %s\n", m); exit(1); }
static void *sym(void *lib, const char *n) { void *p = dlsym(lib, n); if (!p) { fprintf(stderr, "FAIL: missing %s\n", n); exit(1); } return p; }

int main(void)
{
    const char *egl_name = getenv("PB_EGL") ? getenv("PB_EGL") : "libEGL.so";
    const char *gles_name = getenv("PB_GLES") ? getenv("PB_GLES") : "libGLESv2.so";
    void *egl = dlopen(egl_name, RTLD_NOW | RTLD_GLOBAL);
    if (!egl) { fprintf(stderr, "FAIL: dlopen %s: %s\n", egl_name, dlerror()); return 1; }
    void *gles = dlopen(gles_name, RTLD_NOW | RTLD_GLOBAL);
    if (!gles) { fprintf(stderr, "FAIL: dlopen %s: %s\n", gles_name, dlerror()); return 1; }

    void *(*eglGetProcAddress)(const char *) = sym(egl, "eglGetProcAddress");
    EGLDisplay (*eglGetPlatformDisplayEXT)(EGLenum, void *, const EGLint *) = (void *)eglGetProcAddress("eglGetPlatformDisplayEXT");
    if (!eglGetPlatformDisplayEXT) die("eglGetPlatformDisplayEXT (surfaceless platform not built?)");
    EGLBoolean (*eglInitialize)(EGLDisplay, EGLint *, EGLint *) = sym(egl, "eglInitialize");
    const char *(*eglQueryString)(EGLDisplay, EGLint) = sym(egl, "eglQueryString");
    EGLBoolean (*eglBindAPI)(EGLenum) = sym(egl, "eglBindAPI");
    EGLBoolean (*eglChooseConfig)(EGLDisplay, const EGLint *, EGLConfig *, EGLint, EGLint *) = sym(egl, "eglChooseConfig");
    EGLSurface (*eglCreatePbufferSurface)(EGLDisplay, EGLConfig, const EGLint *) = sym(egl, "eglCreatePbufferSurface");
    EGLContext (*eglCreateContext)(EGLDisplay, EGLConfig, EGLContext, const EGLint *) = sym(egl, "eglCreateContext");
    EGLBoolean (*eglMakeCurrent)(EGLDisplay, EGLSurface, EGLSurface, EGLContext) = sym(egl, "eglMakeCurrent");
    EGLint (*eglGetError)(void) = sym(egl, "eglGetError");

    EGLDisplay dpy = eglGetPlatformDisplayEXT(EGL_PLATFORM_SURFACELESS_MESA, NULL, NULL);
    if (!dpy) die("eglGetPlatformDisplayEXT(SURFACELESS) returned no display");
    EGLint maj, min;
    if (!eglInitialize(dpy, &maj, &min)) { fprintf(stderr, "FAIL: eglInitialize 0x%x\n", eglGetError()); return 1; }
    printf("EGL %d.%d  vendor: %s  version: %s\n", maj, min, eglQueryString(dpy, EGL_VENDOR), eglQueryString(dpy, EGL_VERSION));
    if (!eglBindAPI(EGL_OPENGL_ES_API)) die("eglBindAPI");
    const EGLint cfg_attr[] = { EGL_SURFACE_TYPE, EGL_PBUFFER_BIT, EGL_RENDERABLE_TYPE, EGL_OPENGL_ES2_BIT,
        EGL_RED_SIZE, 8, EGL_GREEN_SIZE, 8, EGL_BLUE_SIZE, 8, EGL_ALPHA_SIZE, 8, EGL_NONE };
    EGLConfig cfg; EGLint ncfg = 0;
    if (!eglChooseConfig(dpy, cfg_attr, &cfg, 1, &ncfg) || ncfg < 1) die("eglChooseConfig: no RGBA8 pbuffer ES2 config");
    const EGLint pb_attr[] = { EGL_WIDTH, W, EGL_HEIGHT, H, EGL_NONE };
    EGLSurface surf = eglCreatePbufferSurface(dpy, cfg, pb_attr);
    if (!surf) { fprintf(stderr, "FAIL: eglCreatePbufferSurface 0x%x\n", eglGetError()); return 1; }
    const EGLint ctx_attr[] = { EGL_CONTEXT_CLIENT_VERSION, 2, EGL_NONE };
    EGLContext ctx = eglCreateContext(dpy, cfg, EGL_NO_CONTEXT, ctx_attr);
    if (!ctx) { fprintf(stderr, "FAIL: eglCreateContext 0x%x\n", eglGetError()); return 1; }
    if (!eglMakeCurrent(dpy, surf, surf, ctx)) { fprintf(stderr, "FAIL: eglMakeCurrent 0x%x\n", eglGetError()); return 1; }

    const char *(*glGetString)(GLenum) = sym(gles, "glGetString");
    void (*glClearColor)(GLfloat, GLfloat, GLfloat, GLfloat) = sym(gles, "glClearColor");
    void (*glClear)(GLbitfield) = sym(gles, "glClear");
    void (*glViewport)(GLint, GLint, GLsizei, GLsizei) = sym(gles, "glViewport");
    GLuint (*glCreateShader)(GLenum) = sym(gles, "glCreateShader");
    void (*glShaderSource)(GLuint, GLsizei, const GLchar *const *, const GLint *) = sym(gles, "glShaderSource");
    void (*glCompileShader)(GLuint) = sym(gles, "glCompileShader");
    void (*glGetShaderiv)(GLuint, GLenum, GLint *) = sym(gles, "glGetShaderiv");
    void (*glGetShaderInfoLog)(GLuint, GLsizei, GLsizei *, GLchar *) = sym(gles, "glGetShaderInfoLog");
    GLuint (*glCreateProgram)(void) = sym(gles, "glCreateProgram");
    void (*glAttachShader)(GLuint, GLuint) = sym(gles, "glAttachShader");
    void (*glLinkProgram)(GLuint) = sym(gles, "glLinkProgram");
    void (*glGetProgramiv)(GLuint, GLenum, GLint *) = sym(gles, "glGetProgramiv");
    void (*glUseProgram)(GLuint) = sym(gles, "glUseProgram");
    GLint (*glGetAttribLocation)(GLuint, const GLchar *) = sym(gles, "glGetAttribLocation");
    void (*glVertexAttribPointer)(GLuint, GLint, GLenum, GLboolean, GLsizei, const void *) = sym(gles, "glVertexAttribPointer");
    void (*glEnableVertexAttribArray)(GLuint) = sym(gles, "glEnableVertexAttribArray");
    void (*glDrawArrays)(GLenum, GLint, GLsizei) = sym(gles, "glDrawArrays");
    void (*glFinish)(void) = sym(gles, "glFinish");
    void (*glReadPixels)(GLint, GLint, GLsizei, GLsizei, GLenum, GLenum, void *) = sym(gles, "glReadPixels");
    GLenum (*glGetError)(void) = sym(gles, "glGetError");

    printf("GL renderer: %s  version: %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));

    const char *vs_src = "attribute vec2 p; void main() { gl_Position = vec4(p, 0.0, 1.0); }";
    const char *fs_src = "precision mediump float; void main() { gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0); }";
    GLuint sh[2] = { glCreateShader(GL_VERTEX_SHADER), glCreateShader(GL_FRAGMENT_SHADER) };
    const char *src[2] = { vs_src, fs_src };
    for (int i = 0; i < 2; i++) {
        glShaderSource(sh[i], 1, &src[i], NULL); glCompileShader(sh[i]);
        GLint ok = 0; glGetShaderiv(sh[i], GL_COMPILE_STATUS, &ok);
        if (!ok) { char log[1024]; glGetShaderInfoLog(sh[i], sizeof log, NULL, log); fprintf(stderr, "FAIL: shader %d: %s\n", i, log); return 1; }
    }
    GLuint prog = glCreateProgram(); glAttachShader(prog, sh[0]); glAttachShader(prog, sh[1]); glLinkProgram(prog);
    GLint linked = 0; glGetProgramiv(prog, GL_LINK_STATUS, &linked); if (!linked) die("program link");
    glUseProgram(prog);

    /* Same triangle as M2, in GL's y-up convention: base at the bottom, apex at the top. */
    const GLfloat tri[6] = { -0.9f, -0.9f, 0.9f, -0.9f, 0.0f, 0.9f };
    GLint loc = glGetAttribLocation(prog, "p"); if (loc < 0) die("attrib p");
    glVertexAttribPointer(loc, 2, GL_FLOAT, GL_FALSE, 0, tri); glEnableVertexAttribArray(loc);
    glViewport(0, 0, W, H);
    glClearColor(0, 0, 0, 0); glClear(GL_COLOR_BUFFER_BIT);
    glDrawArrays(GL_TRIANGLES, 0, 3);
    glFinish();
    GLenum err = glGetError(); if (err) { fprintf(stderr, "FAIL: GL error 0x%x after draw\n", err); return 1; }

    static unsigned char px[W * H * 4];
    glReadPixels(0, 0, W, H, GL_RGBA, GL_UNSIGNED_BYTE, px);   /* row 0 = bottom */
    struct { int x, y, inside; } probes[] = {
        { W / 2, H / 2, 1 },      /* center */
        { W / 2, H / 8, 1 },      /* near the wide base (bottom in GL) */
        { 0, H - 1, 0 },          /* top-left corner */
        { W - 4, H / 2, 0 },      /* right of the triangle at mid-height */
        { W / 2, 0, 0 },          /* bottom edge, below the base */
    };
    const unsigned char in_c[4] = { 255, 0, 0, 255 }, out_c[4] = { 0, 0, 0, 0 };
    unsigned bad = 0;
    for (unsigned i = 0; i < sizeof probes / sizeof *probes; i++) {
        const unsigned char *p = px + (probes[i].y * W + probes[i].x) * 4;
        const unsigned char *want = probes[i].inside ? in_c : out_c;
        int ok = 1; for (int c = 0; c < 4; c++) if (abs((int)p[c] - (int)want[c]) > 1) ok = 0;
        printf("pixel(%3d,%3d) %s: got %3u %3u %3u %3u  %s\n", probes[i].x, probes[i].y, probes[i].inside ? "inside " : "outside",
               p[0], p[1], p[2], p[3], ok ? "ok" : "WRONG");
        if (!ok) bad++;
    }
    unsigned red = 0; for (unsigned i = 0; i < W * H; i++) if (px[i * 4] > 200 && px[i * 4 + 1] < 50) red++;
    double frac = (double)red / (W * H);
    printf("red pixels: %u / %u (%.3f, expect ~0.405)\n", red, W * H, frac);
    if (frac < 0.37 || frac > 0.44) { printf("coverage out of range\n"); bad++; }
    printf(bad ? "M5 FAIL: %u checks wrong\n" : "M5 PASS: %u checks wrong\n", bad);
    return bad ? 2 : 0;
}
