# onn-native-gpu

Native, GPU-accelerated Linux-style games on the **onn 4K Pro** (Amlogic S905X5M, Mali-G310, Android 14) with
**no root and a locked bootloader**. The demo: **Super Mario 64 (sm64ex) at its native 30 fps, 1280x720, with
audio and controller**, rendered on the Mali through a from-source Mesa stack and shown on the TV with zero copies.

The box ships a 32-bit-only Android userspace and no Vulkan loader. This repo is how it works anyway:

```
game (arm64, SDL2 + GLES2)
  -> SDL2 offscreen video, EGL surfaceless         patches/sdl
  -> Mesa zink (GL on Vulkan), loads the ICD directly   patches/mesa-csf-v10 005-008
  -> Mesa PanVK on mali_kbase (panvk-kbase-android, g610-v10-csf + fixes 001-004)
  -> dma-buf frames handed to an Android app's HardwareBuffers (zero copy)  present/, app/
  -> app draws them on screen and plays the game's PCM audio
```

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

`onnshell/` is [OnnShell](https://github.com/wyrmbound/onnshell), the launcher app whose zero-copy display shows the
game.

## What is here

| Path | What |
|---|---|
| `patches/mesa-csf-v10/` | Patch series on top of [zenithblue-oss/panvk-kbase-android](https://github.com/zenithblue-oss/panvk-kbase-android) profile `g610-v10-csf`. 001, 002, 004 are driver fixes (offered upstream); 003 is carried but unexercised; 005-008 are only for this runtime (zink without a Vulkan loader, kopper on Android, surfaceless zink, Mesa logs to stderr). |
| `patches/sdl/` | SDL 2.30.11: offscreen driver on EGL surfaceless, plus the present module that renders into the app's dma-bufs (with a depth buffer). |
| `patches/sm64ex/` | sm64ex port for arm64 bionic + GLES2, build recipe (`BUILD.md`) and the launch script. **Bring your own ROM**; nothing here contains game data. |
| `present/sdl_onnm4_present.c` | The present module source (also inside the SDL patch). |
| `app/FrameReceiverView.java` | The early (copying) receiver; the zero-copy app side lives in the launcher app. |
| `tests/` | The milestone harnesses: m1 clear, m2 triangle, m3 streamed frames, m4 zero-copy, m5 EGL/GLES on zink, m6 SDL GLES, m7 SDL audio, m8 SDL gamepad. |
| `docs/` | Design notes, port status, the upstream report with root causes, and the bootloader check. |

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

## Status

Validated on one unit: a full runner pass with every stage green and sm64ex playable with the pad. The bootloader is
vendor-locked (`docs/BOOTLOADER_CHECK.md`), so this route is the route.

## License

Our own code and docs: MIT (see `LICENSE`). The patch files modify third-party projects and fall under those
projects' terms: Mesa (MIT), SDL (zlib), sm64ex (decompilation project; see its repository). No ROM, game assets,
game binaries or Google system binaries are distributed here.
