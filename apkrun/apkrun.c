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

static char data_dir[1024], cache_dir[1100];

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
 * stand-in is enough for the surface. shortcut: ANativeWindow_* calls on it hit the generated libandroid stub
 * (return 0); give it a real object when a port reads the size from it. */
__attribute__((visibility("default"))) void *SDL_GetPointerProperty(uint32_t props, const char *name, void *def)
{
    static char fake_window[64];
    void *(*real)(uint32_t, const char *, void *) = (void *(*)(uint32_t, const char *, void *))dlsym(game, "SDL_GetPointerProperty");
    void *r = real ? real(props, name, def) : def;
    if (!r && name && !strcmp(name, "SDL.window.android.window")) r = fake_window;
    return r;
}

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
    void *h = game = dlopen(lib, RTLD_NOW | RTLD_GLOBAL);
    if (!h) { fprintf(stderr, "apkrun: dlopen failed: %s\n", dlerror()); return 1; }
    int (*sdl_main)(int, char **) = (int (*)(int, char **))dlsym(h, "SDL_main");
    if (!sdl_main) { fprintf(stderr, "apkrun: no SDL_main in %s\n", lib); return 1; }
    fprintf(stderr, "apkrun: calling SDL_main of %s (data %s)\n", lib, data_dir);
    argv[first - 1] = argv[0]; /* the game sees itself as argv[0] */
    int rc = sdl_main(argc - first + 1, argv + first - 1);
    fprintf(stderr, "apkrun: SDL_main returned %d\n", rc);
    return rc;
}
