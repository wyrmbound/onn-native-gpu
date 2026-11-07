# B3.6: sm64ex on the onn 4K Pro (arm64 bionic + PanVK + zink)

Status 2026-11-06: **done and reproducible.** Super Mario 64 (sm64ex `nightly`) runs on the TV at its native 29.9 fps, 1280x720. It renders GLES 2.0 through zink on our from-source PanVK driver and presents zero-copy into OnnShell's HardwareBuffers, with game audio.

## Proof
- Hand run: `runs/m9_mario_hand/` (title screen and Cool, Cool Mountain at 29.9 fps; that run predates the flip fix, so those shots are upside down).
- Full runner pass: `runs/20261106-225349/`. B2 ran at 60 fps, M5, M6 headless, M6 ONNM4 at 59.9 fps, M7 delivered 380,928 audio bytes. M9 ran 90 s: 2,940 frames at 29.9 fps and `client done, 12519424 bytes` of audio (about 98 s of 32 kHz stereo). Its three screencaps differ and are right side up (`m9_sm64_45s.png` = Bowser fight in the attract demo).

## How to rebuild
`sm64ex-patches/BUILD.md`: the Mesa series (`patches/csf-v10/001-008`), the SDL patch (`sdl-patches/`), the sm64ex patch with its two wrappers, then the run step. Every patch was verified the same way: a clean base plus the patch is byte-identical to the working tree that produced the green pass. The binaries staged after the probe cleanup have not been run on the box yet.

## What it took (in order of discovery)
1. **`-DUSE_GLES` was never defined.** The Makefile added it only for `TARGET_RPI`, so SDL asked EGL for desktop GL plus depth 24 on a surfaceless pbuffer, got 0 configs, and every later error was a symptom. Fix: Makefile filter on `USE_GLES` too.
2. **PIE.** linker64 rejects ET_EXEC ("unexpected e_type: 2"), so the game is built with `NO_PIE=0`.
3. **GPU hang at about 249 jobs.** The tiler heap was never renewed on v10, because the work-estimate writer lives only in csf-v11/043. Fixed by `csf-v10/001`; see UPSTREAM_REPORT Finding 4.
4. **Present hook never ran.** `SDL_EGL_CreateContext` makes the context current itself, bypassing the hooked `GL_MakeCurrent`. Fix: the SDL patch also calls the ONNM4 hook from `OFFSCREEN_GLES_CreateContext`.
5. **Upside-down picture.** GL's bottom-up rows appeared as a pure vertical flip. OnnShell's ZeroCopyActivity now draws with `scale(1,-1)`.
6. **Re-launch got no buffers.** `am force-stop` is a no-op on OnnShell (Device Owner + HOME), so every `am start` goes to `onNewIntent`, and a still-alive old `:bridge` blocked the new offer. Helper's app fix uses a fresh service suffix per session (`bridge0`, `bridge1`, …); the runner also kills `:bridge*` before each launch.

## Not verified yet
- **Controller in-game: works.** The DualShock 4 on the kernel hid-playstation driver needs `SDL_GAMECONTROLLERCONFIG` with that layout (SDL 2.30.11 falls back to the old hid-sony mapping, which reads the right stick as the triggers); `run_sm64.sh` exports it.
- Final pass on the probe-free rebuilds (SDL, GL, ICD, sm64) and the newest APK: every stage green; SDL present 60.4 fps, game title screen right side up, game audio delivered, gamepad enumerated and playable with the mapping in `run_sm64.sh`.
