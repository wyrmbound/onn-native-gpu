# Kit instructions for Claude Code

You are setting up native, GPU-accelerated games on a user's **onn 4K Pro** (Amlogic S905X5M, Mali-G310, Android 14)
with this repository. The path is proven end to end; your job is to reproduce it in the most direct way, not to
research alternatives. Work through the stages below in order, show the user each stage's result, and stop at the
first failure with its exact output.

## Hard rules
- **No root, no unlock, no flashing.** The bootloader is vendor-locked (`docs/BOOTLOADER_CHECK.md`). Never run
  `fastboot flashing unlock`, `fastboot flash`, `fastboot erase` or `oem` commands.
- **The user brings their own game ROM.** Never download, request, copy, upload or push a ROM, and never commit or
  publish the built game binary: it contains the game's assets.
- **Ask before anything that changes the box** (installing apps, Device Owner, disabling packages, pushing files) and
  before any download over ~100 MB (the Android GSI is ~1 GB, Mesa and the NDK are large). Say what and why.
- Device Owner can only be set on a box with **no accounts** signed in. If `dpm set-device-owner` refuses, tell the
  user; do not factory-reset the box yourself.
- Never put the user's IPs, serials or paths into files you commit.

## What the user needs
- The box on the same network as a computer with `adb` (Android platform-tools), ADB debugging enabled in the box's
  developer options. USB is optional: the box's **USB-A** socket is its device port, not USB-C.
- A Linux build host: native Linux or WSL2 (we built on aarch64 WSL; x86_64 works with the notes in
  `scripts/10-toolchain.sh`). About 30 GB free.
- An Android 14 **arm64 GSI** zip from Google (source of the 64-bit runtime; the box has no 64-bit system libraries).
- Shizuku (official release APK from github.com/RikkaApps/Shizuku) for running commands as the shell user.
- A DualShock 4 or other gamepad paired to the box.
- For Super Mario 64: their own US ROM as `baserom.us.z64`.

## Stages
Copy `scripts/kit.env.example` to `scripts/kit.env`, fill it in with the user, then:
(Shortcut: the GitHub release "prebuilt" holds our arm64 builds of stages 3 and 4 plus apkrun, vkshim, tests and
tools. Unpack it into `$WORK/pb`, verify `SHA256SUMS`, and skip 30 and 40. Stages 1, 2, 5, 6 and 7 are unchanged.)

1. **Toolchain** `scripts/10-toolchain.sh`: NDK sysroot plus the clang shim.
2. **Runtime bundle** `scripts/20-bionic-bundle.sh`: arm64 linker, libc and the graphics HAL closure from the GSI.
   It must report that every NEEDED library resolves.
3. **Driver** `scripts/30-mesa.sh`: panvk-kbase-android at its pinned commit, `patches/mesa-csf-v10` added as the
   `csf-v10` series, profile `g610-v10-csf`; builds the Vulkan ICD and the GL libraries (EGL, GLESv2, zink).
4. **SDL** `scripts/40-sdl.sh`: SDL 2.30.11 with `patches/sdl` (offscreen on EGL surfaceless + zero-copy present).
5. **Game** `scripts/50-sm64ex.sh`: sm64ex with `patches/sm64ex`, `USE_GLES=1 NO_PIE=0`, from the user's ROM.
6. **Box**: install Shizuku and OnnShell (`onnshell/`, build `assembleFast`, `adb install -r -t`). Provisioning
   OnnShell as Device Owner and home (`onnshell/scripts/provision`) and the debloat are optional; the game path only
   needs OnnShell installed and Shizuku running. Ask the user which they want.
7. **Run** `scripts/60-push-run.sh`, then `scripts/61-tests.sh` for the m1..m8 checks if anything looks wrong.

The user should see the title screen within a few seconds and play with the pad. Options is Start; the PS button
always goes to the Android home and ends the session.

## Known traps (each cost us hours)
- **Shizuku stops on every reboot.** OnnShell restarts it about 20 s after boot over local adb; `60-push-run.sh`
  also starts it if it is down. OnnShell's Shizuku permission survives reboots.
- **`am force-stop` does nothing to OnnShell** when it is Device Owner or the home app. Kill the
  `com.xplsh.onnshell:bridge*` process before relaunching the display activity.
- **Mesa logs nothing** with the bundled liblog; patch 008 sends them to stderr. Read stderr, not logcat.
- **Never set `LIBGL_ALWAYS_SOFTWARE`**: zink then demands a CPU Vulkan device and fails on the Mali.
- **A GPU hang with no fault after a few seconds of a real game** means the tiler-heap renewal patch (001) is
  missing.
- **The box watchdog-reboots a few minutes into a 2D game with the display on** (no kernel log, no DEVICE LOST)
  was read as "patch 013 (one CSG per VkQueue) is missing" on the first, defective unit. On the replacement unit the
  pre-013 driver ran NFS for 20 min without a reboot, so run the CPU memcpy loop on your unit first (Finding 5).
- **The box watchdog-reboots 1-7 minutes into a sustained 3D game even with 013** is the platform, not the driver:
  a CPU-only memcpy loop at ~8.7 GB/s reboots this box the same way, with an empty last_kmsg. Nothing in Mesa fixes
  it; see Finding 6 in `docs/UPSTREAM_REPORT.md` before spending any time on it. 2D titles are unaffected. A 65 W supply
  in place of the stock 10 W one does not help. Capping the GPU clock (devfreq max_freq, reachable through the vendor SystemControl HIDL
  service) does not help either, and DDR frequency is fixed at 1584 MHz; the deaths are not thermal (<= 52 C). Slow
  wide memory walks and holding memory under the game changed nothing, and our unit also rebooted twice while idle.
  The replacement unit ran the same memcpy loop and Mario for 16+ minutes each with no reboot: it was a defective
  box. If a user's box does this, have them run `tools/memhog` for 10 minutes; if it reboots, the unit is bad.
- **A Vulkan-native APK title fails at window or surface creation**: it needs `SDL_VULKAN_LIBRARY` pointing at
  `vkshim/libvulkan.so` (`apkrun/run_apk.sh` sets it), and adb launches of the display activity must pass
  `--ez launch false --ez fresh true` like the home tile, or the old session takes the buffer offer.
- **Corner window (picture in picture):** the game must run with `LD_PRELOAD=$PB/libonngrab.so` (both run scripts set it;
  `40-sdl.sh` builds it). Without it the corner window still works, but every pad press also reaches the app playing
  full screen. `tools/grabtest /dev/input/eventN` prints `busy` while a game holds the pad.
- **The DualShock 4** on this kernel needs the `SDL_GAMECONTROLLERCONFIG` line in `run_sm64.sh`; without it the right
  stick reads as the triggers and Mario crouches.
- **The game must link as PIE** (`NO_PIE=0`); Android's linker refuses `ET_EXEC`.
- **sm64ex's default config is 640x480**; `run_sm64.sh` seeds 1280x720.
- **Fastboot**: Windows on ARM64 cannot bind WinUSB for this device, and USB/IP passthrough resets the port, which
  kills the bootloader's USB. Use an x64 Windows, Linux or macOS host for any read-only fastboot check.

## Where the details are
`docs/UPSTREAM_REPORT.md` (driver root causes), `docs/B3_DESIGN.md` and `docs/B3_6_PORT_STATUS.md` (GL/SDL/game
design and status), `patches/sm64ex/BUILD.md` (game build recipe), `tests/` (milestone harnesses, each prints PASS).
