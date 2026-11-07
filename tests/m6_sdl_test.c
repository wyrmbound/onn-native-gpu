/* M6: an ordinary SDL2 + OpenGL ES 2 program (what a game port looks like) running on the box through
 * SDL offscreen video -> EGL surfaceless -> zink -> PanVK, presenting via the onnm4 path to OnnShell.
 * Draws the M2 triangle over a hue-cycling clear for PB_FRAMES frames (default 600) and prints fps.
 *
 * Run: ONNM4=1 SDL_VIDEODRIVER=offscreen ZINK_VK_ICD=/data/local/tmp/pb/libvulkan_panfrost.so \
 *      MESA_LOADER_DRIVER_OVERRIDE=zink LD_LIBRARY_PATH=/data/local/tmp/pb ./linker64 /data/local/tmp/pb/m6
 * (ZeroCopyActivity must be up; it starts m4h by default, so launch it with --ez launch false and start m6 by hand,
 *  or point its command at m6 later.)
 * Build: aarch64-linux-android29-clang -O1 -DSDL_MAIN_HANDLED m6_sdl_test.c -I<sdl>/include -o m6 -L<sdl>/lib -lSDL2 -ldl -lm
 * Without ONNM4 it still runs headless and reports fps, which is the M5-with-SDL check.
 */
#include <math.h>
#include <stdio.h>
#include <stdlib.h>
#include "SDL.h"

typedef unsigned GLenum, GLuint, GLbitfield; typedef int GLint, GLsizei; typedef float GLfloat; typedef unsigned char GLboolean; typedef char GLchar;
#define GL_COLOR_BUFFER_BIT 0x4000
#define GL_TRIANGLES 0x0004
#define GL_FLOAT 0x1406
#define GL_VERTEX_SHADER 0x8B31
#define GL_FRAGMENT_SHADER 0x8B30
#define GL_RENDERER 0x1F01
#define GL_VERSION 0x1F02

int main(int argc, char **argv)
{
    (void)argc; (void)argv;
    int w = getenv("PB_W") ? atoi(getenv("PB_W")) : 1280, h = getenv("PB_H") ? atoi(getenv("PB_H")) : 720;
    int frames = getenv("PB_FRAMES") ? atoi(getenv("PB_FRAMES")) : 600;
    SDL_SetMainReady(); /* built with -DSDL_MAIN_HANDLED, no SDL2main */
    if (SDL_Init(SDL_INIT_VIDEO) != 0) { fprintf(stderr, "FAIL: SDL_Init: %s\n", SDL_GetError()); return 1; }
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_PROFILE_MASK, SDL_GL_CONTEXT_PROFILE_ES);
    SDL_GL_SetAttribute(SDL_GL_CONTEXT_MAJOR_VERSION, 2);
    SDL_Window *win = SDL_CreateWindow("m6", 0, 0, w, h, SDL_WINDOW_OPENGL);
    if (!win) { fprintf(stderr, "FAIL: SDL_CreateWindow: %s\n", SDL_GetError()); return 1; }
    SDL_GLContext ctx = SDL_GL_CreateContext(win);
    if (!ctx) { fprintf(stderr, "FAIL: SDL_GL_CreateContext: %s\n", SDL_GetError()); return 1; }
    printf("video driver: %s\n", SDL_GetCurrentVideoDriver());

#define P(ret, name, ...) ret (*name)(__VA_ARGS__) = (ret (*)(__VA_ARGS__))SDL_GL_GetProcAddress(#name); \
    if (!name) { fprintf(stderr, "FAIL: no %s\n", #name); return 1; }
    P(const char *, glGetString, GLenum);
    P(void, glClearColor, GLfloat, GLfloat, GLfloat, GLfloat);
    P(void, glClear, GLbitfield);
    P(void, glViewport, GLint, GLint, GLsizei, GLsizei);
    P(GLuint, glCreateShader, GLenum);
    P(void, glShaderSource, GLuint, GLsizei, const GLchar *const *, const GLint *);
    P(void, glCompileShader, GLuint);
    P(GLuint, glCreateProgram, void);
    P(void, glAttachShader, GLuint, GLuint);
    P(void, glLinkProgram, GLuint);
    P(void, glUseProgram, GLuint);
    P(GLint, glGetAttribLocation, GLuint, const GLchar *);
    P(void, glVertexAttribPointer, GLuint, GLint, GLenum, GLboolean, GLsizei, const void *);
    P(void, glEnableVertexAttribArray, GLuint);
    P(void, glDrawArrays, GLenum, GLint, GLsizei);
    printf("GL renderer: %s  version: %s\n", glGetString(GL_RENDERER), glGetString(GL_VERSION));

    const char *vs = "attribute vec2 p; void main() { gl_Position = vec4(p, 0.0, 1.0); }";
    const char *fs = "precision mediump float; void main() { gl_FragColor = vec4(1.0, 0.0, 0.0, 1.0); }";
    GLuint v = glCreateShader(GL_VERTEX_SHADER), f = glCreateShader(GL_FRAGMENT_SHADER);
    glShaderSource(v, 1, &vs, NULL); glCompileShader(v); glShaderSource(f, 1, &fs, NULL); glCompileShader(f);
    GLuint prog = glCreateProgram(); glAttachShader(prog, v); glAttachShader(prog, f); glLinkProgram(prog); glUseProgram(prog);
    const GLfloat tri[6] = { -0.9f, -0.9f, 0.9f, -0.9f, 0.0f, 0.9f };
    GLint loc = glGetAttribLocation(prog, "p");
    glVertexAttribPointer(loc, 2, GL_FLOAT, 0, 0, tri); glEnableVertexAttribArray(loc);
    glViewport(0, 0, w, h);

    Uint64 t0 = SDL_GetPerformanceCounter();
    for (int i = 0; i < frames; i++) {
        float hue = (i % 180) / 180.0f * 6.0f, x = 1.0f - fabsf(fmodf(hue, 2.0f) - 1.0f), r = 0, g = 0, b = 0;
        switch ((int)hue) { case 0: r = 1; g = x; break; case 1: r = x; g = 1; break; case 2: g = 1; b = x; break;
                            case 3: g = x; b = 1; break; case 4: r = x; b = 1; break; default: r = 1; b = x; }
        glClearColor(r * 0.25f, g * 0.25f, b * 0.25f, 1.0f); glClear(GL_COLOR_BUFFER_BIT);
        glDrawArrays(GL_TRIANGLES, 0, 3);
        SDL_GL_SwapWindow(win);
        SDL_Event e; while (SDL_PollEvent(&e)) if (e.type == SDL_QUIT) frames = i;
    }
    double dt = (double)(SDL_GetPerformanceCounter() - t0) / SDL_GetPerformanceFrequency();
    printf("%d frames %dx%d in %.3f s = %.1f fps\nM6 PASS\n", frames, w, h, dt, frames / dt);
    SDL_GL_DeleteContext(ctx); SDL_DestroyWindow(win); SDL_Quit();
    return 0;
}
