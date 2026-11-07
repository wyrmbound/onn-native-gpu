# B3 design: running recompiled arm64 games on the onn box

**Status: design notes written before the work; the outcome is recorded at the end (B3 DONE).**

## The one-line architecture

```
game (arm64 ELF, bionic)
  -> SDL2 (arm64 bionic, custom/offscreen video + evdev input)
    -> EGL (surfaceless) + GLES2/3
      -> zink  (GL-on-Vulkan, Mesa gallium)
        -> PanVK  (our from-source driver)  [DONE: M1/M2]
          -> mali kbase -> Mali-G310
  frames out via M4 path (app-owned dma-buf) -> OnnShell SurfaceView -> TV  [DONE: M4 built]
  audio -> (later) PCM over socket -> 32-bit app AudioTrack
  input <- /dev/input/event* read as shell (shell is in group 1004 'input')
```

The bottom two layers (PanVK on kbase, and the dma-buf present path) are the parts we already proved. B3 is the GL + SDL + glue stack on top, plus per-game cross-compilation.

## What the tree already gives us (recon)

- **zink IS in the tree** (`src/gallium/drivers/zink`), so GL-on-Vulkan is available — zink turns GLES/GL into Vulkan calls against an ICD, and our ICD (PanVK) works.
- **EGL surfaceless platform IS present** (`src/egl/drivers/dri2/platform_surfaceless.c`, plus `platform_device.c`). That is the windowless EGL we need — there is no 64-bit `libnativewindow`/`libgui` on the box, so the normal android EGL platform is out.
- The current build is **Vulkan-only**: `build-android.sh` passes `-Dgallium-drivers= -Dvulkan-drivers=panfrost -Degl=disabled -Dgles1/2=disabled -Dopengl=false`. So B3 needs a **new, separate build config**, not a tweak to the shipping one.
- Profiles are all Vulkan (`g610-v10-csf.json` etc.); there is no GL/zink profile — we add one.

## Layer-by-layer plan, status, and blockers

### B3.1 — Build zink + GLES + EGL(surfaceless) for arm64 bionic  [NEW, medium-large]
New meson config alongside the Vulkan build: `-Dgallium-drivers=zink -Dvulkan-drivers=panfrost -Dgles2=enabled -Dgles1=enabled -Degl=enabled -Dopengl=true -Dplatforms=surfaceless -Dglvnd=disabled -Degl-native-platform=surfaceless` (exact set TBD), same clang-18 + r27d shim as the Vulkan build.
- Output: `libEGL.so`, `libGLESv2.so`, `libgallium_dri.so`/zink, loaded by the game via our bundled bionic.
- Blockers to expect (same class as the Vulkan bring-up): android-stub gaps (no real `libnativewindow`/`cutils`), dri loader path (zink is loaded as a gallium dri driver; the loader must find it without the Android HAL), and EGL device/surfaceless enumeration finding our PanVK ICD. zink also wants `VK_EXT_*` host-image/External features — PanVK's support set needs a pass.
- Risk: medium-large. This is the second-biggest bring-up after PanVK itself, but zink is mature and the surfaceless path is well-trodden on desktop; the novelty is bionic + no-HAL + our ICD.

### B3.2 — Windowless present: EGL/zink output -> M4 dma-buf -> OnnShell  [reuses M4, medium]
The game renders to the EGL "surface". With surfaceless there is no on-screen surface, so we either (a) render to an FBO backed by an app-owned dma-buf imported as an EGLImage/GL texture (cleanest, fully zero-copy, reuses the M4 import we are already building), and `eglSwapBuffers` becomes "signal M4 that buffer i is ready"; or (b) render to a normal FBO and blit/export per frame (a copy, like B1). Prefer (a): bind the M4 HardwareBuffer dma-buf as a GL render target via `EGL_ANDROID_image_native_buffer`/`EGL_EXT_image_dma_buf_import`, draw, hand to OnnShell. This is the same buffer rotation as M4, one layer up.
- Blocker: EGLImage-from-dma-buf import must work in our EGL/zink build (needs `EGL_EXT_image_dma_buf_import` + zink importing the dma-buf as a Vulkan image — which is exactly the M4 `bo_import_fd` path we are already validating). So B3.2 is gated on M4 working.

### B3.3 — SDL2 on arm64 bionic, no Java glue  [NEW, large]
SDL2's Android backend hard-depends on the Java `SDLActivity`; we have no JVM in the renderer process. Options:
- Use SDL's **`offscreen` video driver** (`SDL_VIDEODRIVER=offscreen`) which creates a windowless EGL context — pairs directly with our surfaceless EGL. But offscreen presents nowhere, so we patch its `SwapWindow` to drive the B3.2 present (or write a tiny custom SDL video backend "onnm4" that does EGL-surfaceless + M4 handoff). ~a few hundred lines.
- **Input** via SDL's evdev path reading `/dev/input/event*` directly: the box's shell uid is in group `1004 (input)`, so it can open the event devices (confirmed from the M3 recon). SDL's Linux evdev backend (`SDL_INPUT_LINUXEV`) should work once built for bionic; gamepad/remote mapping TBD.
- **Audio**: no 64-bit audio HAL on the box. Defer: stub SDL audio first (games run silent), then a PCM-over-socket bridge to a 32-bit AudioTrack in OnnShell (same transport family as M4). Medium, later.
- Cross-compile SDL2 for arm64 bionic with the r27d NDK sysroot (SDL supports an Android-without-Gradle build with effort, or a plain autotools/cmake cross-build targeting bionic).

### B3.4 — The game itself  [per-game, large]
sm64-style ports (sm64ex, sm64-port, sm64coopdx) are the common case: C, SDL2, GL (fixed-function-ish via a GL compat shim or GLES). They need the original ROM as `baserom.us.z64` (owner supplies; we do not distribute it). Cross-compile the port for arm64 bionic against our SDL2 + GLES. Each game is its own port effort; shader/GL-version mismatches (many ports target desktop GL 2.1/3.3, not GLES) are the main friction — zink exposes desktop GL, which actually helps here (zink can give GL 4.6, unlike a native GLES-only driver).

## Honest size estimate

B3 is the **largest phase so far** — bigger than M1+M2+M3 combined, and naturally several milestones / sessions:
- B3.1 (zink/EGL/GLES bionic build): comparable to the PanVK bring-up itself. The single biggest unknown.
- B3.2 (EGLImage-from-dma-buf present): small-medium, but gated on M4.
- B3.3 (SDL2 bionic + offscreen/custom backend + evdev): large.
- B3.4 (first game): large, per-game, plus the ROM/licensing caveat.
- B3.5 (audio bridge): medium, deferrable.

Realistic first concrete milestone, before any game: **a hand-written GLES2 triangle/`glClear` through SDL2(offscreen)->EGL(surfaceless)->zink->PanVK, presented via M4 to the TV.** That proves B3.1+B3.2+B3.3 end-to-end with ~200 lines of our code and no game/ROM, exactly like M2 was the minimal proof for rendering. Only after that green do we attempt a real port.

## Open questions for the owner
- Which specific game first? (sm64ex is the likely default; confirm, and that he has the base ROM.) Different ports pull different GL versions and deps.
- Is silent-first acceptable (audio bridge deferred to B3.5)?

## Dependencies on in-flight work
- B3.2/B3.4 presentation is gated on **M4 (zero-copy dma-buf import) working** — that is the current build, box-parked. B3.1 (the zink build) is independent and can start now; it needs no box, only WSL + the shim.

## Recommendation
Start B3.1 (the zink/EGL/GLES bionic build) as the next real work once the owner says go — it is box-independent, it is the gating unknown, and it reuses the exact toolchain/shim that already works for PanVK. Hold B3.3/B3.4 until B3.1 compiles and B3.2 can be proven against a working M4.

---

## PROGRESS (2026-11-06)
- **B3 DONE: sm64ex runs on the TV** (29.9 fps at 1280x720, audio, zero-copy present, right side up). Full green runner pass `runs/20261106-225349`. Status, proof and the six things it took are in `B3_6_PORT_STATUS.md`. To rebuild from clean sources, use `sm64ex-patches/BUILD.md` (Mesa `patches/csf-v10/001-008`, `sdl-patches/`, the sm64ex patch). Every patch has been verified to reproduce the working tree byte for byte. Open: the controller in-game (owner presses Start), and one box pass with the probe-free rebuilds and the newest APK.
- Older notes below are kept for history; some (e.g. "the sm64ex repo was DMCA'd, not cloned") were overtaken: we did clone and build `sm64pc/sm64ex` `nightly`.
- **B3.1 built** (first attempt): `libEGL.so` 387K, `libGLESv2.so` 60K, `libgallium_dri.so` 19.5M (zink). All NEEDED already in the box bundle. zink patched to load our ICD directly (`ZINK_VK_ICD`) with a deferred device-GIPA fetch. panvk advertises every extension zink wants. M5 harness (`m5h`) built. Queued for the box.
- **B3.3a built**: `libSDL2-2.0.so.0` for arm64 bionic, NEEDED = libm/libdl/libc only. Video = offscreen+dummy, audio = dummy, joystick = linux evdev, threads = pthread. Two fixes needed: (a) `-U__ANDROID__` on the C/CXX flags — the shim targets `aarch64-linux-android29` so `__ANDROID__` is defined and SDL's `#ifdef __ANDROID__` source pulls in `Android_JNI_*`/`__android_log_write` that aren't built; suppressing it makes SDL take the Linux paths matching `CMAKE_SYSTEM_NAME=Linux`. (b) desktop `SDL_OPENGL` was off (no `GL/gl.h` in the NDK sysroot) — **fixed**: copied `GL/{gl,glext,glcorearb}.h` from `work/mesa/include/GL/` into the r27d sysroot's `usr/include/GL/` (KHR/khrplatform.h was already there) and relinked → `SDL_OPENGL=ON`. SDL's offscreen driver makes desktop-GL contexts through EGL (`EGL_OPENGL_API`), so nothing else changes. GLES also on.

## sm64ex dependency check (B3.4)
Done from knowledge rather than cloning the decomp (the `sm64ex` repo was DMCA'd; the dependency set is well-known, and a clone adds legal/size risk for a recon — a precise Makefile read can be added later). No ROM, no assets touched, ever. Target family: `sm64-port`/`sm64ex` (C, SDL2, a `gfx_opengl` rendering backend). `sm64ex-coop`/`sm64coopdx` is more actively maintained but heavier (networking, Lua, more deps) — **recommend plain `sm64ex` (or `sm64-port`) for the first target**: single-player, smallest dependency surface.

What it needs vs. what we have:
- **SDL2** (window + GL context + controller + audio): HAVE (B3.3a).
- **Rendering backend / GL:** the port's RAPI is `gfx_opengl`, buildable two ways:
  - Desktop GL 2.1 via **GLEW** to resolve entry points → a NEW dep (cross-build GLEW for bionic) *or* patch `gfx_opengl.c` to use `SDL_GL_GetProcAddress` instead of GLEW. zink exposes desktop GL ≥ 2.1, so the GL path is in range.
  - **GLES2** (`make USE_GLES=1`): uses our `libGLESv2.so` directly, **no GLEW**. **Recommended** — it sidesteps the GLEW cross-build entirely and matches our confirmed GLES2 build.
- **GL entry-point loading / no `libGL.so`:** our GL build emits **no `libGL.so`** (Mesa only produces it with GLX, which we disabled) — fine for EGL clients, but a port that links `-lGL` and calls `glXxx` symbols *directly* won't link against us. The port must resolve GL entry points at runtime via `SDL_GL_GetProcAddress` (which maps to `eglGetProcAddress`). `sm64ex`'s `gfx_opengl.c` does that on Windows through GLEW but links `libGL` directly on Linux, so our config needs one of: GLEW built in EGL mode, **glad generated with an EGL loader** (preferred — one generated header/source, no runtime dependency, smaller than cross-building GLEW), or a tiny `libGL.so` shim that forwards every symbol to `eglGetProcAddress`. **Recommendation: glad (EGL loader), generated once.** With the GLES2 path this is moot (GLESv2 symbols link directly); it only matters for the desktop-GL build of the port.
- **libpthread / libm / libdl / libc:** HAVE (bionic; SDL already pulls pthread).
- **Audio:** SDL audio → dummy (silent) for now. HAVE. Real audio is the later PCM-over-socket bridge (B3.5).
- **Input:** SDL joystick over evdev (`/dev/input/event*`, shell is in group `input`). HAVE.
- **Host build tools:** `make`, `python3` (asset extraction), and the port builds its N64 tools (`n64graphics`, `skyconv`, …) from source with the *host* compiler. HAVE in WSL.
- **Assets / ROM:** extracted from the owner's `baserom.us.z64` by the Makefile (python3) and **baked into the binary** — the stock Makefile does **not** separate the code build from assets, so a full build needs the ROM. The owner supplies it at build time; we never handle it. The dependency list above is deliverable without it.

Net new work for the B3.4 milestone (beyond B3.1–B3.3):
1. Cross-compile the port for arm64 bionic with the shim + SDL2 + **GLES2** (skip GLEW). Note: SDL still redefines `main`→`SDL_main` on this NDK target (it defines `__ANDROID_API__` even with `-U__ANDROID__`) and we build no `libSDL2main`, so the port must either build with **`-DSDL_MAIN_HANDLED`** and call `SDL_SetMainReady()` before `SDL_Init` (what m6 does), or `#include "SDL_main.h"` and provide an `SDL_main`. sm64ex's Makefile links `SDL2main` on some targets — drop that link and use `SDL_MAIN_HANDLED` instead.
2. Present path: the port's `SDL_GL_SwapWindow` → our custom SDL offscreen backend (**B3.3b**) → M4 app-owned dma-buf → OnnShell.
3. Input already covered by SDL evdev.
4. Owner provides the ROM for the asset step.
Size: a milestone of its own, gated on M5 (GL stack proven on the box) and B3.3b (the present glue), which is gated on M4.
