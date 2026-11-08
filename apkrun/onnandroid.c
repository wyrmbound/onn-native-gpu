/*
 * libonnandroid: the Android window calls some ports reference, as no-ops. On the box a game's libandroid.so is an
 * empty stand-in (the system one is 32-bit), so a port that links ANativeWindow_* failed to load at all (DKR-R,
 * 2026-11-11). Our SDL draws offscreen and hands frames to OnnShell, so the game never gets a real window here:
 * fromSurface returns no window, and the rest accept that. run_apk.sh preloads this for every APK game.
 * It also carries __emutls_get_address (the NDK's own emutls.c.o from libclang_rt.builtins-aarch64-android.a, linked
 * in whole): on a phone it comes from somewhere the app loaded first, and SoH, 2Ship and DKR-R failed to load without it.
 * And __cxa_demangle (the NDK's libc++abi.a, linked in and hidden except for that one name): on a phone it comes from
 * the system libc++ behind libGLESv3.so, and 2Ship failed to load without it. onnandroid.map keeps the rest private.
 * Build: llvm-ar x .../libclang_rt.builtins-aarch64-android.a emutls.c.o
 *        clang --target=aarch64-linux-android29 -O2 -fPIC -shared -o libonnandroid.so onnandroid.c emutls.c.o
 *              -Wl,-u,__cxa_demangle <ndk sysroot>/usr/lib/aarch64-linux-android/libc++abi.a
 *              -Wl,--version-script=onnandroid.map   (all one command)
 */
#include <stddef.h>
#include <stdint.h>

typedef struct ANativeWindow ANativeWindow;

__attribute__((visibility("default"))) ANativeWindow *ANativeWindow_fromSurface(void *env, void *surface) { (void)env; (void)surface; return NULL; }
__attribute__((visibility("default"))) void ANativeWindow_acquire(ANativeWindow *w) { (void)w; }
__attribute__((visibility("default"))) void ANativeWindow_release(ANativeWindow *w) { (void)w; }
__attribute__((visibility("default"))) int32_t ANativeWindow_getWidth(ANativeWindow *w) { (void)w; return 0; }
__attribute__((visibility("default"))) int32_t ANativeWindow_getHeight(ANativeWindow *w) { (void)w; return 0; }
