# onn-native-gpu

Native, GPU-accelerated Linux-style games on the **onn 4K Pro** (Amlogic S905X5M, Mali-G310, Android 14) with
**no root and a locked bootloader**. The demo: **Super Mario 64 (sm64ex) at its native 30 fps, 1280x720, with
audio and controller**, rendered on the Mali through a from-source Mesa stack and shown on the TV with zero copies.

The box ships a 32-bit-only Android userspace and no Vulkan loader. This repo is how it works anyway:

```
game (arm64, SDL2 + GLES2)
  -> SDL2 offscreen video, EGL surfaceless         patches/sdl
  -> Mesa zink (GL on Vulkan), loads the ICD directly   patches/mesa-csf-v10 005-008
  -> Mesa PanVK on mali_kbase (panvk-kbase-android, g610-v10-csf + fixes 001-004, 009-013)
  -> dma-buf frames handed to an Android app's HardwareBuffers (zero copy)  present/, app/
  -> app draws them on screen and plays the game's PCM audio
```

Vulkan-native games (for example 64-bit Android game APKs built on SDL3) skip zink: `vkshim/libvulkan.so` is a small
Vulkan loader for this box that loads PanVK directly and adds an ONNM4 window system, which presents into the same
zero-copy buffers. `apkrun/` runs such an APK's native `libmain.so` as a shell process (the box's 32-bit userspace
cannot install 64-bit APKs), with a JNI stand-in for the Java side the game expects.

The 64-bit processes run as the adb shell user through a bundled arm64 bionic runtime (linker64, libc and the
graphics HAL client libraries) taken from an Android 14 GSI. The app side is a launcher app holding Shizuku; a small
Shizuku service passes the buffers' dma-buf fds to the renderer over a unix socket.

## Use it with Claude Code

```bash
git clone --recursive https://github.com/wyrmbound/onn-native-gpu
```
```bash
cd onn-native-gpu && claude
```

Then ask: *"Set up native games on my onn 4K Pro with this kit. Walk me through each stage and ask before you change
anything on the box."* Claude Code reads `CLAUDE.md`, which has the stages, the rules and every trap we hit. You bring
the box, a Linux or WSL build host, an Android 14 arm64 GSI, Shizuku, a gamepad and your own game ROM.

## Skip the long builds: prebuilt binaries

The [prebuilt release](../../releases) has every arm64 binary we built ourselves (PanVK ICD with patches 001-013,
Mesa EGL/GLESv2/zink, patched SDL2 and SDL3, `vkshim/libvulkan.so`, `apkrun`, the m1-m8 tests, the diagnosis tools
and the launch scripts), with SHA256 sums. Unpack it into `$WORK/pb` and skip stages 3 and 4 (`30-mesa.sh`,
`40-sdl.sh`). You still run 1 and 2 (the toolchain is needed for the NEEDED check, and the 64-bit runtime comes from
your own GSI, which we do not redistribute), 5 for your own Mario build, and 6 and 7 as usual. The release holds no
game, ROM, game assets or Google system files.

`onnshell/` is [OnnShell](https://github.com/wyrmbound/onnshell), the launcher app whose zero-copy display shows the
game.

## What is here

| Path | What |
|---|---|
| `patches/mesa-csf-v10/` | Patch series on top of [zenithblue-oss/panvk-kbase-android](https://github.com/zenithblue-oss/panvk-kbase-android) profile `g610-v10-csf`. 001, 002, 004 are driver fixes (offered upstream); 003 is carried but unexercised; 005-008 are only for this runtime (zink without a Vulkan loader, kopper on Android, surfaceless zink, Mesa logs to stderr); 009-011 are csf-v11 077/085/098 adapted for v10 (cross-subqueue sync scope, syncs in CSF event memory, heap ops); 012 fails a wait fast on a CSF group error; 013 uses one kbase CSG with three CSIs per VkQueue, which fixes the watchdog hang below. |
| `patches/sdl/` | SDL 2.30.11: offscreen driver on EGL surfaceless, plus the present module that renders into the app's dma-bufs (with a depth buffer). |
| `patches/sm64ex/` | sm64ex port for arm64 bionic + GLES2, build recipe (`BUILD.md`) and the launch script. **Bring your own ROM**; nothing here contains game data. |
| `present/sdl_onnm4_present.c` | The present module source (also inside the SDL patch). |
| `app/FrameReceiverView.java` | The early (copying) receiver; the zero-copy app side lives in the launcher app. |
| `vkshim/` | The Vulkan loader shim and ONNM4 WSI (`build.sh` regenerates the dispatch from Mesa's `vk.xml`), plus `wsitest`/`vktest`. |
| `apkrun/` | Runner for 64-bit Android game APKs: loads `libmain.so` with a JNI shim, `run_apk.sh` launch script, `apkinstall.sh`. SDL3 shims for titles that ask for haptic/sensor or the Android window pointer. |
| `tools/` | Diagnosis tools from the freeze hunt: `gpubench` (timed GPU clear loop), `csgprobe` (reads kbase CSG slot counts), `keepalive` (steady GPU submits), `memhog` (CPU memcpy DDR load), `memtest` (slow wide memory walk and a memory shield, for the pattern-vs-traffic runs). |
| `tests/` | The milestone harnesses: m1 clear, m2 triangle, m3 streamed frames, m4 zero-copy, m5 EGL/GLES on zink, m6 SDL GLES, m7 SDL audio, m8 SDL gamepad. |
| `docs/` | Design notes, port status, the upstream report with root causes, the bootloader check, and `NFS_RUNS.md` (every freeze A/B run). |

## Findings worth knowing

See `docs/UPSTREAM_REPORT.md` for evidence. In short:

1. `KBASE_IOCTL_MEM_ALIAS` returns ENOMEM on this kbase (r44p1, CSF UK 1.20); the descriptor ring now single-maps.
2. The CRC zero-init path freed a kbase SAME_VA image BO with a raw `munmap`, so the GPU faulted mid-draw.
3. The v10 CSF profile carried the tiler-heap renewal policy without its accounting, so heavy scenes hang the GPU
   after a few seconds with no fault.
4. SDL's offscreen driver needs EGL device enumeration, which Mesa's device platform never maps to zink; using the
   surfaceless platform fixes it. SDL creates the context current through an unhooked path, so a present hook must
   also run at context creation.
5. The DualShock 4 on the newer kernel `hid-playstation` driver needs an explicit SDL mapping on SDL 2.30.
6. Three HIGH priority CSGs per VkQueue competing with the system's groups (display on) wedge the GPU during
   firmware group rotation, and the vendor kernel's GPU reset then hard-locks the box (watchdog reboot, no log).
   Patch 013 (one CSG with three CSIs per VkQueue) fixes it for 2D titles (27 min and counting).
7. **Sustained 3D titles still watchdog-reboot this box after 1 to 7 minutes, and it is not the driver.** A CPU-only
   memcpy loop at about 8.7 GB/s, with no GPU work at all, reboots it the same way with an empty last_kmsg, panel on
   or off, on the stock 10 W supply or a 65 W one. It is a random SoC lockup whose odds rise with DDR traffic.
   1080p output stretched one game to 7.5 minutes; nothing in userspace prevents it. Capping the GPU clock, a 65 W
   supply, a slow 1 GB memory walk and holding memory under the game changed nothing, and the unit also rebooted
   twice while idle with nothing of ours running, so our one unit is probably marginal hardware; it went back under
   warranty on 2026-11-08. See Finding 6 and `docs/NFS_RUNS.md`.

## Status

Validated on one unit: a full runner pass with every stage green and sm64ex playable with the pad. The bootloader is
vendor-locked (`docs/BOOTLOADER_CHECK.md`), so this route is the route. A Vulkan-native SDL3 title (a 64-bit
Android game APK) renders through `vkshim` + `apkrun` at about 49 fps. 2D titles run indefinitely; sustained 3D
titles are limited by the platform lockup above.

## License

Our own code and docs: MIT (see `LICENSE`). The patch files modify third-party projects and fall under those
projects' terms: Mesa (MIT), SDL (zlib), sm64ex (decompilation project; see its repository). `apkrun/jni.h` is the
Android Open Source Project's header (Apache 2.0). No ROM, game assets,
game binaries or Google system binaries are distributed here.
