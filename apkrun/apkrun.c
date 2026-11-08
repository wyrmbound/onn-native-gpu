/* apkrun: run an Android SDL2 port's own arm64 game code (lib/arm64-v8a/libmain.so) on the onn box's bundled
 * 64-bit runtime, with no Android app runtime. Our libSDL2 (offscreen + ONNM4 present) stands in for the APK's,
 * under the name libSDL2.so; the few Android-only SDL calls are answered here.
 * SDL3 ports work the same way: our libSDL3.so in pb stands in for the APK's.
 * Usage: LD_LIBRARY_PATH=<game lib dir>:<pb> linker64 apkrun <game lib dir>/libmain.so [args...]
 * Env:   APKRUN_DATA=<dir> (what SDL_AndroidGetInternalStoragePath returns; default: cwd; cache path = <dir>/cache)
 * Build: aarch64-linux-android29-clang -O1 -rdynamic -I. apkrun.c jnishim.c -o apkrun -ldl
 * JNI: the game gets Helper's fake JNIEnv/Activity (jnishim.c); every call logs once and returns a harmless value. */
#include <dlfcn.h>
#include "jnishim.h"
#include <stdio.h>
#include <stdlib.h>
#include <string.h>
#include <unistd.h>
#include <stdbool.h>
#include <stdint.h>
#include <sys/stat.h>
#include <signal.h>
#include <sys/time.h>
#include <ucontext.h>

static char data_dir[1024], cache_dir[1100];

/* APKRUN_PROF=<seconds>: where is a stalled game burning CPU? The box has no debugger for 64-bit code, so a CPU-time
 * timer interrupts whichever thread is running and prints where it was: library+offset for pc, lr and the frame
 * chain. Read the offsets with llvm-objdump or llvm-addr2line. Diagnostic only; off unless the variable is set. */
static void prof_where(const char *tag, uintptr_t a)
{
    Dl_info d;
    if (dladdr((void *)a, &d) && d.dli_fname)
        fprintf(stderr, "  %s %s+0x%lx %s\n", tag, strrchr(d.dli_fname, '/') ? strrchr(d.dli_fname, '/') + 1 : d.dli_fname,
                (unsigned long)(a - (uintptr_t)d.dli_fbase), d.dli_sname ? d.dli_sname : "");
    else fprintf(stderr, "  %s 0x%lx\n", tag, (unsigned long)a);
}
static void prof_tick(int sig, siginfo_t *si, void *uc_)
{
    (void)sig; (void)si;
    static int left = 12;
    if (left <= 0) return;
    left--;
    ucontext_t *uc = uc_;
    uintptr_t fp = uc->uc_mcontext.regs[29], sp = uc->uc_mcontext.sp;
    fprintf(stderr, "apkrun prof: tid %d\n", gettid());
    prof_where("pc", uc->uc_mcontext.pc);
    prof_where("lr", uc->uc_mcontext.regs[30]);
    for (int i = 0; i < 10 && fp > sp && fp - sp < (8u << 20) && !(fp & 7); i++) {
        uintptr_t next = ((uintptr_t *)fp)[0];
        prof_where("fp", ((uintptr_t *)fp)[1]);
        if (next <= fp) break;
        fp = next;
    }
}
static void prof_start(void)
{
    const char *s = getenv("APKRUN_PROF");
    if (!s || atoi(s) <= 0) return;
    struct sigaction sa = { .sa_sigaction = prof_tick, .sa_flags = SA_SIGINFO | SA_RESTART };
    sigaction(SIGPROF, &sa, NULL);
    struct itimerval it = { { atoi(s), 0 }, { atoi(s), 0 } };
    setitimer(ITIMER_PROF, &it, NULL);
}

/* Exported from the executable (-rdynamic) so bionic resolves the game's imports of these here. */
__attribute__((visibility("default"))) const char *SDL_AndroidGetInternalStoragePath(void) { return data_dir; }
__attribute__((visibility("default"))) const char *SDL_AndroidGetExternalStoragePath(void) { return data_dir; }
__attribute__((visibility("default"))) int SDL_AndroidGetExternalStorageState(void) { return 3; /* read + write */ }
__attribute__((visibility("default"))) void *SDL_AndroidGetJNIEnv(void) { return jnishim_env(); }
__attribute__((visibility("default"))) void *SDL_AndroidGetActivity(void) { return jnishim_activity(); }
/* Games that call JNI_OnLoad-style setup look up the VM through SDL too. */
__attribute__((visibility("default"))) void *SDL_AndroidGetJavaVM(void) { return jnishim_vm(); }
/* SDL3 renamed them (SDL_GetAndroid*); our libSDL3 is a Linux build, so these come from here too. */
__attribute__((visibility("default"))) const char *SDL_GetAndroidInternalStoragePath(void) { return data_dir; }
__attribute__((visibility("default"))) const char *SDL_GetAndroidExternalStoragePath(void) { return data_dir; }
__attribute__((visibility("default"))) void *SDL_GetAndroidJNIEnv(void) { return jnishim_env(); }
__attribute__((visibility("default"))) void *SDL_GetAndroidActivity(void) { return jnishim_activity(); }
/* The rest of SDL 3.2's Android API (SDL_system.h), with the answers an Android TV box would give. */
__attribute__((visibility("default"))) uint32_t SDL_GetAndroidExternalStorageState(void) { return 3; /* READ | WRITE */ }
__attribute__((visibility("default"))) const char *SDL_GetAndroidCachePath(void) { return cache_dir; }
__attribute__((visibility("default"))) int SDL_GetAndroidSDKVersion(void) { return 34; }
__attribute__((visibility("default"))) bool SDL_IsChromebook(void) { return false; }
__attribute__((visibility("default"))) bool SDL_IsDeXMode(void) { return false; }
__attribute__((visibility("default"))) bool SDL_IsTV(void) { return true; } /* overrides our Linux libSDL3's false */
__attribute__((visibility("default"))) int SDL_IsAndroidTV(void) { return 1; } /* SDL2 name, SDL_bool */
__attribute__((visibility("default"))) void SDL_SendAndroidBackButton(void) {}
__attribute__((visibility("default"))) bool SDL_SendAndroidMessage(uint32_t command, int param) { (void)command; (void)param; return true; }
__attribute__((visibility("default"))) bool SDL_ShowAndroidToast(const char *message, int duration, int gravity, int xoffset, int yoffset)
{ (void)duration; (void)gravity; (void)xoffset; (void)yoffset; fprintf(stderr, "apkrun: toast: %s\n", message ? message : ""); return true; }
typedef void (*SDL_RequestAndroidPermissionCallback)(void *userdata, const char *permission, bool granted);
__attribute__((visibility("default"))) bool SDL_RequestAndroidPermission(const char *permission, SDL_RequestAndroidPermissionCallback cb, void *userdata)
{ if (cb) cb(userdata, permission, true); return true; }
/* SDL 3.4 API (the game was built against newer headers than our 3.2.30): no PNG decoder here, so fail like SDL does. */
__attribute__((visibility("default"))) void *SDL_LoadPNG(const char *file)
{
    bool (*set_error)(const char *, ...) = (bool (*)(const char *, ...))dlsym(RTLD_DEFAULT, "SDL_SetError");
    fprintf(stderr, "apkrun: SDL_LoadPNG(%s) unsupported, returning NULL\n", file ? file : "");
    if (set_error) set_error("SDL_LoadPNG unsupported in apkrun");
    return NULL;
}

/* Our SDL2/SDL3 are built with no sensor or haptic driver, and SDL_Init fails outright when a game asks for them
 * (NFS Most Wanted does): drop those bits. A TV box has no sensors to miss. Same bit values in SDL2 and SDL3. */
static void *game;
static int init_without(uint32_t flags, const char *name, void *self)
{
    int (*real)(uint32_t) = (int (*)(uint32_t))dlsym(game, name);
    if (!real || (void *)real == self) { fprintf(stderr, "apkrun: no %s in the game's SDL\n", name); return -1; }
    return real(flags & ~0x9000u); /* SDL_INIT_HAPTIC 0x1000 | SDL_INIT_SENSOR 0x8000 */
}
__attribute__((visibility("default"))) int SDL_Init(uint32_t flags) { return init_without(flags, "SDL_Init", (void *)SDL_Init); }
__attribute__((visibility("default"))) int SDL_InitSubSystem(uint32_t flags) { return init_without(flags, "SDL_InitSubSystem", (void *)SDL_InitSubSystem); }

/* An Android SDL3 port expects SDL.window.android.window (the ANativeWindow*) on its window; our offscreen window
 * has none and NFS Most Wanted gives up on NULL. Our WSI's vkCreateAndroidSurfaceKHR ignores the window, so a
 * stand-in is enough for the surface. */
static char fake_window[64];
static int win_w = 1920, win_h = 1080;
__attribute__((visibility("default"))) void *SDL_GetPointerProperty(uint32_t props, const char *name, void *def)
{
    void *(*real)(uint32_t, const char *, void *) = (void *(*)(uint32_t, const char *, void *))dlsym(game, "SDL_GetPointerProperty");
    void *r = real ? real(props, name, def) : def;
    if (!r && name && !strcmp(name, "SDL.window.android.window")) r = fake_window;
    return r;
}

/* The SDL2 form of the same need: a port asks SDL for the Android window (SDL_GetWindowWMInfo) to make its own Vulkan
 * surface (DKR-R's renderer), and our offscreen SDL answers "That operation is not supported". Hand out the same
 * stand-in, and let the window size calls answer with the SDL window's size (they come before libonnandroid's zeros,
 * because the executable is searched first). */
struct wminfo { uint8_t version[3]; int subsystem; struct { void *window, *surface; } android; };
__attribute__((visibility("default"))) int SDL_GetWindowWMInfo(void *window, void *info)
{
    void (*size)(void *, int *, int *) = (void (*)(void *, int *, int *))dlsym(game, "SDL_GetWindowSize");
    if (size && window) size(window, &win_w, &win_h);
    ((struct wminfo *)info)->subsystem = 9; /* SDL_SYSWM_ANDROID */
    ((struct wminfo *)info)->android.window = fake_window;
    ((struct wminfo *)info)->android.surface = NULL;
    return 1;
}
__attribute__((visibility("default"))) void *ANativeWindow_fromSurface(void *env, void *surface) { (void)env; (void)surface; return fake_window; }
__attribute__((visibility("default"))) int32_t ANativeWindow_getWidth(void *w) { (void)w; return win_w; }
__attribute__((visibility("default"))) int32_t ANativeWindow_getHeight(void *w) { (void)w; return win_h; }

/* A port's prompts (SDL_ShowMessageBox) have no screen to appear on here, and our SDL fails them, which leaves the
 * port with an answer it never set (SoH and 2Ship then quit on "No OTR files found. Generate one now?"). Log the
 * prompt and press its default button. One exception: "ROM Extracted. Extract another?" defaults to Yes and would
 * extract the same ROM a second time, so a prompt asking for "another" gets the other button.
 * SDL2 and SDL3 lay these structs out alike but disagree on the return value: 0 or true for success. */
struct msgbutton { uint32_t flags; int id; const char *text; };
struct msgbox { uint32_t flags; void *window; const char *title, *message; int numbuttons; const struct msgbutton *buttons; };
static int msg_ok(void) { return dlsym(game, "SDL_CreateRGBSurface") ? 0 : 1; } /* an SDL2-only name */
__attribute__((visibility("default"))) int SDL_ShowMessageBox(const void *data, int *buttonid)
{
    const struct msgbox *m = data;
    int another = m->message && strstr(m->message, "another"), pick = -1;
    for (int i = 0; i < m->numbuttons && pick < 0; i++)
        if (((m->buttons[i].flags & 1) != 0) != another) pick = i; /* 1 = the Return key's default button */
    if (pick < 0) pick = 0;
    fprintf(stderr, "apkrun: prompt \"%s\": %s -> %s\n", m->title ? m->title : "", m->message ? m->message : "",
            m->numbuttons > 0 && m->buttons[pick].text ? m->buttons[pick].text : "(no buttons)");
    if (buttonid && m->numbuttons > 0) *buttonid = m->buttons[pick].id;
    return msg_ok();
}
__attribute__((visibility("default"))) int SDL_ShowSimpleMessageBox(uint32_t flags, const char *title, const char *message, void *window)
{
    (void)flags; (void)window;
    fprintf(stderr, "apkrun: message \"%s\": %s\n", title ? title : "", message ? message : "");
    return msg_ok();
}

/* 2Ship asks its start-up questions ("No O2R files found. Generate one now?", "ROMs found in application directory.
 * Would you like to process them?", "... Use this rom?") in a popup of its own, which wants a controller press
 * before there is a game to play. Those three get their first button pressed for
 * them, like the SDL prompts above; every other popup reaches the game untouched (they confirm things like deleting
 * a save). The arguments are libc++ objects, read here by layout: strings by reference (long form: bit 0 of byte 0,
 * text pointer at +16), std::function by reference with its target at +0x20 and operator() in vtable slot 6. */
#define BENGUI_POPUP _ZN6BenGui13RegisterPopupENSt6__ndk112basic_stringIcNS0_11char_traitsIcEENS0_9allocatorIcEEEES6_S6_S6_NS0_8functionIFvvEEES9_
#define STR_(x) #x
#define STR(x) STR_(x)
static const char *cxx_str(const void *s)
{
    return (*(const unsigned char *)s & 1) ? *(const char *const *)((const char *)s + 16) : (const char *)s + 1;
}
__attribute__((visibility("default"))) void BENGUI_POPUP(void *title, void *msg, void *b1, void *b2, void *yes, void *no)
{
    const char *m = cxx_str(msg);
    void **target = *(void ***)((char *)yes + 0x20);
    if (target && (strstr(m, "Generate one now") || strstr(m, "Would you like to process them") || strstr(m, "Use this rom"))) {
        fprintf(stderr, "apkrun: prompt \"%s\": %s -> %s\n", cxx_str(title), m, cxx_str(b1));
        ((void (*)(void *))(*(void ***)target)[6])(target);
        return;
    }
    void (*real)(void *, void *, void *, void *, void *, void *) =
        (void (*)(void *, void *, void *, void *, void *, void *))dlsym(game, STR(BENGUI_POPUP));
    if (real) real(title, msg, b1, b2, yes, no);
}

/* The GL window (see SDL_CreateWindow below) keeps its size: later fullscreen and resize requests succeed and do
 * nothing. Other windows are passed through. */
static void *gl_window;
__attribute__((visibility("default"))) int SDL_SetWindowFullscreen(void *window, long how)
{
    int (*real)(void *, long) = (int (*)(void *, long))dlsym(game, "SDL_SetWindowFullscreen");
    return window == gl_window || !real ? msg_ok() : real(window, how);
}
__attribute__((visibility("default"))) int SDL_SetWindowSize(void *window, int w, int h)
{
    int (*real)(void *, int, int) = (int (*)(void *, int, int))dlsym(game, "SDL_SetWindowSize");
    return window == gl_window || !real ? msg_ok() : real(window, w, h);
}

/* Our SDL gives a port the launcher's buffer as "framebuffer 0", but only to ports that take glBindFramebuffer from
 * SDL_GL_GetProcAddress. A port that links GL directly (SoH, 2Ship) bound the real framebuffer 0, and its picture
 * never reached the screen. Send the linked call through SDL's as well. */
__attribute__((visibility("default"))) void glBindFramebuffer(unsigned target, unsigned fb)
{
    static void (*bind)(unsigned, unsigned);
    if (!bind) {
        void *(*gpa)(const char *) = (void *(*)(const char *))dlsym(game, "SDL_GL_GetProcAddress");
        if (gpa) bind = (void (*)(unsigned, unsigned))gpa("glBindFramebuffer");
    }
    if (bind) bind(target, fb);
}

#ifndef APKRUN_TRACE
/* A port whose window or GL context cannot be made usually goes on to crash on the NULL with no word of why (SoH).
 * Say why. Six integer arguments carry both SDL2's (title, x, y, w, h, flags) and SDL3's (title, w, h, flags). */
__attribute__((visibility("default"))) void *SDL_CreateWindow(const char *title, long a, long b, long c, long d, long e)
{
    void *(*real)(const char *, long, long, long, long, long) = (void *(*)(const char *, long, long, long, long, long))dlsym(game, "SDL_CreateWindow");
    const char *(*err)(void) = (const char *(*)(void))dlsym(game, "SDL_GetError");
    /* Android only has OpenGL ES and its SDL defaults to the ES profile, so a port never asks for it. Our SDL
     * defaults to desktop GL, which our EGL does not offer ("Couldn't find matching EGL config", SoH). Ask for ES
     * on the port's behalf: attribute SDL_GL_CONTEXT_PROFILE_MASK (21 in SDL2, 20 in SDL3) = ES (4). */
    int sdl3 = msg_ok();
    int (*set)(int, int) = (int (*)(int, int))dlsym(game, "SDL_GL_SetAttribute");
    int gl = ((sdl3 ? c : e) & 2 /* SDL_WINDOW_OPENGL */) != 0;
    if (gl && set) set(sdl3 ? 20 : 21, 4);
    /* A GL port draws into the launcher's 1280x720 buffers (our SDL binds them as the window framebuffer), so its
     * window must be that size and stay that size. Our SDL's offscreen display is 1024x768, and a port that goes
     * fullscreen, as Android ports do, was resized to it and drew a picture of the wrong size (SoH). Make the
     * window 1280x720, not fullscreen (flag 1 in SDL2 and SDL3, 0x1001 for SDL2's desktop kind). */
    if (gl) { if (sdl3) { a = 1280; b = 720; c &= ~1L; } else { c = 1280; d = 720; e &= ~0x1001L; } }
    void *r = real ? real(title, a, b, c, d, e) : NULL;
    if (gl) gl_window = r;
    if (!r) fprintf(stderr, "apkrun: SDL_CreateWindow(%s, %ld, %ld, %ld, %ld, 0x%lx) failed: %s\n", title ? title : "", a, b, c, d, e, err ? err() : "?");
    return r;
}
__attribute__((visibility("default"))) void *SDL_GL_CreateContext(void *window)
{
    void *(*real)(void *) = (void *(*)(void *))dlsym(game, "SDL_GL_CreateContext");
    const char *(*err)(void) = (const char *(*)(void))dlsym(game, "SDL_GetError");
    void *r = real ? real(window) : NULL;
    if (!r) fprintf(stderr, "apkrun: SDL_GL_CreateContext failed: %s\n", err ? err() : "?");
    return r;
}
#endif

#ifdef APKRUN_TRACE
/* Diagnostic build (apkrun.trace): log SDL's Vulkan glue calls and SDL_GetError() to $APKRUN_TRACE_SDL.
 * The 4-argument wrapper also serves SDL2's 3-argument SDL_Vulkan_CreateSurface: x3 is simply unused there. */
static FILE *sdl_trace(void) { static FILE *f; if (!f) { const char *p = getenv("APKRUN_TRACE_SDL"); f = p ? fopen(p, "a") : NULL; if (f) setvbuf(f, NULL, _IONBF, 0); } return f; }
static const char *sdl_error(void) { const char *(*e)(void) = (const char *(*)(void))dlsym(game, "SDL_GetError"); return e ? e() : "?"; }
__attribute__((visibility("default"))) int SDL_Vulkan_CreateSurface(void *window, void *instance, const void *allocator, void **surface)
{
    int (*real)(void *, void *, const void *, void **) = (int (*)(void *, void *, const void *, void **))dlsym(game, "SDL_Vulkan_CreateSurface");
    int r = real ? real(window, instance, allocator, surface) : 0;
    if (sdl_trace()) fprintf(sdl_trace(), "SDL_Vulkan_CreateSurface(window %p, instance %p) -> %d, error \"%s\"\n", window, instance, r, sdl_error());
    return r;
}
__attribute__((visibility("default"))) const char *const *SDL_Vulkan_GetInstanceExtensions(uint32_t *count)
{
    const char *const *(*real)(uint32_t *) = (const char *const *(*)(uint32_t *))dlsym(game, "SDL_Vulkan_GetInstanceExtensions");
    const char *const *r = real ? real(count) : NULL;
    if (sdl_trace()) { fprintf(sdl_trace(), "SDL_Vulkan_GetInstanceExtensions -> %u:", count ? *count : 0); for (uint32_t i = 0; r && count && i < *count; i++) fprintf(sdl_trace(), " %s", r[i]); fprintf(sdl_trace(), " error \"%s\"\n", sdl_error()); }
    return r;
}
__attribute__((visibility("default"))) bool SDL_GetWindowSizeInPixels(void *window, int *w, int *h)
{
    bool (*real)(void *, int *, int *) = (bool (*)(void *, int *, int *))dlsym(game, "SDL_GetWindowSizeInPixels");
    bool r = real ? real(window, w, h) : false;
    if (sdl_trace()) fprintf(sdl_trace(), "SDL_GetWindowSizeInPixels(%p) -> %d, %dx%d, error \"%s\"\n", window, r, w ? *w : -1, h ? *h : -1, sdl_error());
    return r;
}
__attribute__((visibility("default"))) void *SDL_CreateWindow(const char *title, int w, int h, uint64_t flags)
{
    void *(*real)(const char *, int, int, uint64_t) = (void *(*)(const char *, int, int, uint64_t))dlsym(game, "SDL_CreateWindow");
    void *r = real ? real(title, w, h, flags) : NULL;
    if (sdl_trace()) fprintf(sdl_trace(), "SDL_CreateWindow(%s, %dx%d, flags 0x%llx) -> %p, error \"%s\"\n", title ? title : "", w, h, (unsigned long long)flags, r, sdl_error());
    return r;
}
#endif

/* Some ports take their screen from their own Activity instead of from SDL: Android's Java side calls a native
 * "here is the Surface" function each time it makes one, and the renderer refuses to start until that has happened
 * (DKR-R: "Vulkan presentation preflight failed"). There is no Java side here, so make that call once, with the
 * stand-in. One name per port that needs it. */
static void publish_surface(void *h)
{
    static const char *const names[] = { "Java_io_github_thatguymcd_dkrr_DkrActivity_publishSurface" };
    for (size_t i = 0; i < sizeof names / sizeof *names; i++) {
        void (*f)(void *, void *, void *) = (void (*)(void *, void *, void *))dlsym(h, names[i]);
        if (f) { fprintf(stderr, "apkrun: %s\n", names[i]); f(jnishim_env(), jnishim_activity(), fake_window); }
    }
}

int main(int argc, char **argv)
{
    /* The game may re-exec itself (Doom ports do) with its own argv, which has no library path: remember it in
     * the environment so the re-exec'd runner still knows which game to load. */
    const char *lib = NULL; int first = 1;
    size_t n = argc > 1 ? strlen(argv[1]) : 0;
    if (n > 3 && strcmp(argv[1] + n - 3, ".so") == 0) { lib = argv[1]; first = 2; setenv("APKRUN_LIB", lib, 1); }
    else lib = getenv("APKRUN_LIB");
    if (!lib) { fprintf(stderr, "usage: apkrun <path/to/libmain.so> [args]\n"); return 2; }
    const char *d = getenv("APKRUN_DATA");
    if (d) snprintf(data_dir, sizeof data_dir, "%s", d); else if (!getcwd(data_dir, sizeof data_dir)) data_dir[0] = 0;
    snprintf(cache_dir, sizeof cache_dir, "%s/cache", data_dir);
    mkdir(cache_dir, 0755); /* EEXIST is fine */
    setvbuf(stdout, NULL, _IOLBF, 0); /* the log is a file: full buffering lost the game's last words when it aborted */
    void *h = game = dlopen(lib, RTLD_NOW | RTLD_GLOBAL);
    if (!h) { fprintf(stderr, "apkrun: dlopen failed: %s\n", dlerror()); return 1; }
    int (*sdl_main)(int, char **) = (int (*)(int, char **))dlsym(h, "SDL_main");
    if (!sdl_main) { fprintf(stderr, "apkrun: no SDL_main in %s\n", lib); return 1; }
    publish_surface(h);
    prof_start();
    fprintf(stderr, "apkrun: calling SDL_main of %s (data %s)\n", lib, data_dir);
    argv[first - 1] = argv[0]; /* the game sees itself as argv[0] */
    int rc = sdl_main(argc - first + 1, argv + first - 1);
    fprintf(stderr, "apkrun: SDL_main returned %d\n", rc);
    return rc;
}
