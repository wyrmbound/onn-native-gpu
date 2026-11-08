# NFS Most Wanted (Recompa "BLACK" SDL3 port, build 87b951e) on the onn box

2026-11-07. First Vulkan-native title through the from-source stack: libvulkan.so (onn shim + ONNM4 WSI)
-> libvulkan_panfrost.so (csf-v10 001-013). Intro movie and attract footage render on the TV at ~49 fps
presented, RSS flat at 423 MB / 657 maps over 75 s.

## What blocked it, in order

| Symptom | Cause | Fix |
|---|---|---|
| `SDL_Init: SDL not built with sensor support`, SDL_main returns 1 | game asks for SDL_INIT_SENSOR; our SDL2/SDL3 are built with SDL_SENSOR=OFF, SDL_HAPTIC=OFF | apkrun exports `SDL_Init`/`SDL_InitSubSystem` that drop HAPTIC\|SENSOR (apkrun.c) |
| `app: no window: Failed to load Vulkan Portability library` | SDL3 offscreen Vulkan loader tries `libvulkan.so.1` (Linux default) | `SDL_VULKAN_LIBRARY="$PB/libvulkan.so"` in run_apk.sh |
| `d3d: renderer init failed` right after vkCreateInstance, then SIGSEGV at 0x10 in thread "render" | game reads `SDL.window.android.window` (ANativeWindow*) from the window properties and gives up on NULL | apkrun exports `SDL_GetPointerProperty` returning a stand-in for that key (our WSI ignores the window) |
| `vkCreateHeadlessSurfaceEXT failed: VK_ERROR_SURFACE_LOST_KHR` after 30 s | adb launch without `--ez launch false`: ZeroCopyActivity ran m4h, which took the single buffer offer | launch like the tile: `am start -n com.xplsh.onnshell/.display.ZeroCopyActivity --ez launch false --ez fresh true --es game mostwanted` |

Facts checked on the way (vkprobe, pb/vkprobe): the game's four device extensions (swapchain, create_renderpass2,
depth_stencil_resolve, dynamic_rendering) plus the dynamicRendering feature create a device fine; this G310 SKU
reports textureCompressionBC=1 with BC1/BC2/BC3 sampled, D24S8/D32S8 as depth-stencil attachments.

## Diagnostics added (opt-in builds, production binaries unchanged)

- `vkshim/libvulkan.so.trace`: `-DONN_TRACE`; with `ONN_VK_TRACE=<file>` every Vulkan call the app makes
  (GIPA/GDPA results redirected to our logging trampolines) goes to that file. Swap it in as pb/libvulkan.so
  for one run; the game dlopens `libvulkan.so` by name, so SDL_VULKAN_LIBRARY alone does not reach it.
- `apkrun/apkrun.trace`: `-DAPKRUN_TRACE`; with `APKRUN_TRACE_SDL=<file>` logs SDL_CreateWindow,
  SDL_GetWindowSizeInPixels, SDL_Vulkan_GetInstanceExtensions, SDL_Vulkan_CreateSurface and SDL_GetError.
- The game's stderr goes to logcat (tag BLACK) through a lossy pump; the file traces exist because of that.

## Open

- Touch overlay (d-pad, A/B, gear) is drawn on screen: the port thinks it is on a phone. Game setting or
  SDL hint to find.
- Heartbeat of the first long run: /data/local/tmp/nfs_heartbeat.txt (5 s: pid, maps, RSS). A Vulkan-direct
  3D title is the differential for the ~6 min 3D watchdog death seen with Mario (zink).

## Hang A/B runs on NFS (2026-11-07 20:05-20:33, tile launch path, heartbeat 5 s)

| Run | Driver | Renewal | Outcome |
|---|---|---|---|
| N1 | 013 (0ac69e67) | default (128 / 65536) | SoC reboot at ~3:00 (19:47:10 last heartbeat, maps flat 651-658) |
| N3 | 013 | off (INTERVAL=0 WORK=0) | heap full at 0:44: kernel "Queue group to be terminated, couldn't handle the OoM event", DEVICE_LOST, frame frozen, process alive, **no reboot in 9 min** (GPU idle after 0:44, so this is not a survival of 3D load) |
| N4 | 013 | every submit (INTERVAL=1) | SoC reboot at ~0:22 (last heartbeat 20:16:12) |
| N5 | .v1no011 (95d494df, stock heap ops) | default | SoC reboot between 1:18 and 1:34 |
| N5b | .v1no011 | off | SoC reboot at ~2:30 (7620 frames at 2:23, last heartbeat 20:24:15); no OOM, no "Invalid Heap statistics" |
| Nb (display put to sleep at 20 s, 8 min, about 20:25 to 20:33) | 013 | default (128 / 65536) | invalid: panel off destroyed the surface and SIGSTOPped the game (post-13:55 APK taskAffinity); M4 frame count frozen at 1020 from the moment the panel went off |

Reading: more renewals die sooner (N4 << N1), but no renewal at all with firmware chunk recycling (N5b) still dies at
the N1 timescale, so renewal is an accelerant, not the sole cause. N3's survival only shows an idle GPU does not reboot.
Every configuration that keeps rendering 3D for minutes dies; the 2D title (G1, Tyrian 27 min) did not. Display-off
runs on the post-13:55 APK (Nb here, X4 in runs/hang) froze the game and say nothing about survival.
Heartbeats: heartbeat-N*.txt in this folder.

## Late runs (2026-11-07 22:16 onward): the cause is outside the driver

All runs are panel on and use the 013 driver (0ac69e67) unless noted. Harness: scratchpad nrun.sh. Hog: onn-pathb/memhog.c, run through pb/kald.

| Run | Setup | Outcome |
|---|---|---|
| Nka | NFS plus the gpubench-style keepalive (pb/kald) | died between 161 and 177 s, 7,680 frames; keepalive normal until then (10.6k submits/min) |
| Nsm | NFS, skip_movies = true (read only from data/mostwanted.toml; /sdcard/MostWanted and android.toml are ignored; confirmed in BLACK) | died between 54 and 69 s, 2,940 frames |
| Nhog | NFS plus 4 shell busy loops at ~94% CPU each | died between 132 and 147 s, 7,260 frames; game stayed at ~60 fps |
| Nmed | NFS on .v1med (ad2f22ae, 013 + MEDIUM priority) | died between 146 and 162 s, 7,380 frames |
| Nmem | NFS plus memhog, 3 memcpy threads (8.7 GB/s alone, 6.7 to 7.7 beside the game) | **died between 23 and 39 s, 300 frames (game ~13 fps)** |
| Mem1 | memhog alone, 3 threads, no game, no GPU work, panel woken at start | **inconclusive**: the test PC lost its network link in the same window. The box log shows memhog alive until 130 s (adb lost at 95 s) and no watchdog boot before the 22:53 cold boot |
| **Mem1b** | **memhog alone, 3 threads, panel verified ON every 5 s, network stable** | **watchdog reboot at about 325 to 340 s** (last heartbeat 23:12:31 mScreenState=ON; memhog 8.67 to 8.80 GB/s to the end; sys.boot.reason=watchdog; new dropbox SYSTEM_LAST_KMSG "watchdog_reboot", kernel silent). Logs: mhb-Mem1b.txt, mem-Mem1b.txt |
| **Mem2off** | memhog alone, 3 threads, **panel verified OFF** every 5 s | **watchdog reboot at about 50 to 60 s** (memhog 8.54 to 8.89 GB/s; last heartbeat 23:16:25 mScreenState=OFF; dropbox 23:17:53 watchdog_reboot). Panel state is not a necessary ingredient. Logs: mhb-Mem2off.txt, mem-Mem2off.txt |
| **Mem3one** | memhog alone, **1 thread**, panel verified ON | **watchdog reboot within about 5 to 10 s of start** (no memhog sample yet; last heartbeat 23:19:41; dropbox 23:20:56 watchdog_reboot; soc_thermal 52.7 C afterwards). Lowest load, fastest death: does not fit a simple bandwidth-rate curve. Logs: mhb-Mem3one.txt, mem-Mem3one.txt |
| **N1080** | NFS, output 1920x1080 at 59.94 (settings user_preferred_resolution_* plus reboot), launched at uptime 318 s on a quiet box | **died between 440 and 455 s, 22,920 frames**: the longest default-driver NFS life tonight (usual 150 to 180 s), but no survival |
| **Nrp33** | NFS, render_percent = 33 (game-confirmed), 2160p output, launched at uptime 322 s on a quiet box | **died between 38 and 54 s, 1,860 frames**: no mitigation |
| **P1** (2026-11-08) | memhog alone, 3 threads, panel verified ON, **new 65 W USB-C supply** (factory one is 10 W), launched at uptime ~310 s, quiet box | **watchdog reboot at about 285 to 294 s** (memhog 8.67 to 8.75 GB/s to the end; last heartbeat 00:10:42; dropbox 00:12:07 watchdog_reboot, kernel silent). Same as on the 10 W supply: **power delivery ruled out**. P2 and P3 skipped as planned. Logs: mhb-P1.txt, mem-P1.txt |
| 1080p | `cmd display set-user-preferred-display-mode` | refused for shell: SecurityException "Package android does not belong to 2000" in SettingsProvider; nothing changed |

Kernel side: runs/nfs/kmsg/ holds every dropbox entry (SYSTEM_LAST_KMSG, SYSTEM_BOOT, SYSTEM_AUDIT, system_server_anr).
- The 20 watchdog boots left nothing but boot lines in last_kmsg. That buffer does keep ERR and EMERG lines: clean reboots show "reboot: Restarting system", and the sysrq reboots show the full INFO shutdown.
- So before any freeze there was no oops, soft lockup, RCU stall or mali dev_err.
- One unrelated line, from the boot that started at 20:25: "DMC SEC READ CHECK ERROR ... port:ARM_A53".

Reading (from Mem1b): CPU-only DDR traffic of about 8.7 GB/s locks the SoC in about 5.5 min, NFS alone in 2.5 to 3 min, both together in under 40 s (additive load against one platform limit), so a sustained 3D game is just one more heavy DDR client. This is a platform fault (DDR, fabric or power delivery), not PanVK or kbase. Panel off (lower CPU clocks, no 4K scanout) is the only state that has survived so far. Next: memhog with the panel off; memhog with 1 thread (dose-response); then 1080p output and NFS render_percent 33 as mitigations.

### Uptime at launch vs time to death (2026-11-07)

Uptime at launch is the box uptime at the first poll minus the poll time. Death is the window between the last good poll and the lost poll.

| Clock | Run | Uptime at launch | Died between |
|---|---|---|---|
| 20:59 | Na (heap 400/400, renewal off) | ~1613 s | 347-363 s |
| 21:31 | .v1hs | ~1622 s | 223-238 s |
| 21:38 | 013 | ~175 s | 223-239 s |
| 21:42 | .v1tb (TILEBUF_DIV=4) | ~72 s | 100-115 s |
| 21:45 | .v1pad | ~66 s | 100-115 s |
| 21:58 | 013 | ~241 s | 239-255 s |
| 22:05 | 013 PANVK_DEBUG=sync | ~201 s | 161-177 s |
| 22:15 | Nka (keepalive) | ~400 s | 161-177 s |
| 22:23 | Nsm (skip_movies) | ~341 s | 54-69 s |
| 22:28 | Nhog (busy loops) | ~137 s | 132-147 s |
| 22:33 | Nmed | ~146 s | 146-162 s |
| 22:41 | Nmem (NFS + memhog) | ~393 s | 23-39 s |
| 22:44 | Mem1 (memhog, network drop) | ~131 s | ~130-140 s by box log |
| 23:07 | Mem1b (memhog 3t, on) | ~851 s | 325-341 s |
| 23:15 | Mem2off (memhog 3t, off) | ~124 s | 50-62 s |
| 23:19 | Mem3one (memhog 1t, on) | ~126 s | 5-10 s |
| 23:3x | N1080 (quiet box) | ~318 s | 440-455 s |
| 23:46 | Nrp33 (quiet box) | ~322 s | 38-54 s |

Reading:
- Weak clustering. The two launches at about 70 s uptime died at 100-115 s, and the two at about 1600 s lasted 230-360 s. For memhog alone, launches at about 125 s died in 5-55 s, and the one at 851 s lasted 330 s.
- But fresh boot is neither necessary nor sufficient: Nka at 400 s died at 170 s, and Nrp33 at 322 s on a quiet box died at 50 s.
- Spread is wide under the same config (013 NFS: 54 to 455 s). Treat it as a random SoC lockup whose odds rise with DDR load, with no safe ceiling measured.

## Conclusion (2026-11-07, power row added 2026-11-08)

The 3D watchdog death is a platform fault of this onn 4K Pro (Amlogic S905X5M), not PanVK, kbase, zink or anything in
our stack. Proof: a 3-thread CPU memcpy loop at ~8.7 GB/s, with no game and no GPU work, reboots the box by watchdog on
its own (Mem1b 5.5 min, Mem2off 55 s with the panel off, Mem3one 1 thread within 10 s), and the kernel's last_kmsg is
empty for every watchdog boot (CPUs stop without printing: bus/DDR fabric or power-delivery level). Every driver knob
we could turn left the outcome unchanged; every display-path and clock explanation was tested and cleared. Deaths are
random with odds rising with DDR traffic: NFS alone 54-455 s, NFS + memhog <40 s, 2D never.

What this means for games on this box:
- 2D and light titles (Tyrian class) are fine: 27 min and counting on 013.
- Sustained 3D titles (Mario, NFS) are a 1-7 minute experience until a platform-level mitigation exists. 1080p output
  roughly doubled NFS's life (7.5 min) but did not save it; render scale 33% did nothing.
- Nothing in userspace can read or set DDR/GPU DVFS, thermal, display mode or kernel printk on this build (all denied
  to shell), so the remaining mitigation is the vendor's firmware. A different power supply was tested on 2026-11-08 (65 W
  USB-C in place of the stock 10 W): memhog alone still died at about 4:50 (row P1), so power delivery is ruled out.
- Hypothesis still open, unproven: launching heavy work right after boot dies sooner (weak clustering in the table).

**Replacement unit, 2026-11-09 (same firmware and binaries).** Same harness, panel on, attract loop, `SDL_AUDIODRIVER=dummy` (no effect: apkrun forces SDL's disk audio), 20 min each:

| Run | Driver | Outcome |
|---|---|---|
| N013 | 001-013 (0ac69e67) | survived 1200 s, 59,160 frames, max 63 C |
| Npre | 001-012 (730608c3, three CSGs per queue) | survived 1200 s, 58,260 frames, max 63 C |

The lockup above was the unit. NFS on this unit runs at about 50 fps for as long as tested on either driver.

On-screen touch controls: the port draws them only while SDL reports no gamepad, or for about 8 s after a screen touch
(`touch::Shown`). With the DS4 mapped they never appear on the TV, and no setting is needed. The in-game TOUCH CONTROLS menu
has opacity, size, steering and stick mode for phones.

Driver work that stands regardless: 013 (one CSG per queue) and the NFS launch fixes (apkrun SDL_Init/SDL_GetPointerProperty
shims, SDL_VULKAN_LIBRARY, tile launch flags). Finding 5's before/after should be re-validated on a board without this
lockup before it is presented upstream as proven.

## Capped runs (Haxxor Jr, 2026-11-08 10:02-11:14)

Brief: `docs/capped-run-brief.md` (owner: "Yes, run capped"), amended by Helper 64 (settle gate: uptime >= 600 s and
soc_thermal flat/falling x3 before each capped launch). Tool: OnnShell's SystemControl HIDL hook (`docs/sysctl-probe.md`);
GPU cap = `/sys/class/devfreq/fd000000.valhall/max_freq`, read back before every launch. Heartbeat 5 s (game pid, valhall
cur_freq, aml_ddr/freq, soc_thermal from `dumpsys thermalservice` "Current temperatures from HAL"; soc_thermal is the only
zone). Every launch: `ZeroCopyActivity --ez launch false --ez fresh true --es game <id>`, nobody touching the pad unless noted.

| Run | Title | Cap (max_freq read back) | Outcome | GPU cur_freq seen | DDR | soc_thermal | Last heartbeat | File |
|---|---|---|---|---|---|---|---|---|
| C0 | NFS MW | none (852000000) | **watchdog reboot at ~7:11** (launch 10:02:43, uptime ~9.8 h) | 852 MHz on all 79 beats | 1584 MHz throughout | not logged | 10:09:54 | cap-C0.txt |
| C1 a1 | NFS MW | 666666666 -> **666667000** | invalid: cap not applied, GPU 852 MHz (kHz round-up lands above the 666666666 OPP, driver picks 852); picture froze ~10:24:45 (GPU idle, process alive), killed 10:25:05 | 852 then 285 | 1584 | 49-50 C | 10:25:06 | cap-C1.txt.attempt1 |
| (owner) | NFS MW | none (852000000 restored) | owner relaunched by hand 10:25:15, no heartbeat; **watchdog reboot ~10:27:28** (~2:10) | - | - | - | - | - |
| C1 a2 | NFS MW | **666666000** | ran at 500 MHz, one 852 MHz beat at 10:39:25 = owner pressed the remote (input-boost preheat passes the cap); guard stopped it, killed 10:39:44. From launch on, kernel logs `get_devfreq_mali_freq, idx:-1` every ~1 s (driver's own table has no 666666000), persisting after max_freq was restored | 500 (+1x 852) | 1584 | 47.5-49.6 C | 10:39:25 | cap-C1-attempt2.txt |
| C1 a3 | NFS MW | 666666000 (same boot as a2) | invalid: **GPU_SHAREABILITY_FAULT** in AS1-AS4 6 s after launch, no image, sound only; GPU idle 285; killed 10:43:53 | 285 | 1584 | 47-48 C | 10:43:28 | cap-C1-attempt3.txt |
| (reboot) | - | - | owner-approved `adb reboot` 10:45:47 to clear the idx:-1 state; after boot idx:-1 = 0, max_freq 852000000 | - | - | - | - | - |
| **C1** | NFS MW | **500000000** (exact OPP, kHz-aligned; owner-approved substitute for 666) | **GPU_SHAREABILITY_FAULT at 11:00:27 (~4:28)**: MESA "CSF group 0 fatal error 0x00c00488", vkGetSemaphoreCounterValue -4 (device lost), rendering stopped, process alive, **no reboot**; stopped by hand 11:01:47. No idx:-1 | 500 MHz until the fault, then 285 | 1584 | 49.8-52.6 C | 11:01:26 | cap-C1.txt, shots/C1 |
| **M1** | Mario (games/sm64, zink on PanVK) | **500000000** (same boot as C1, after C1's fault) | **watchdog reboot at ~7:38** (launch 11:04:24); no GPU fault, rendered to the end | 400-500 MHz, never above | 1584 | 49.8-51.5 C (51.2 C at death) | 11:12:02 | cap-M1.txt, shots/M1 |

Also: after C0's death the box rebooted **twice more while idle** (boots 10:11:11, 10:12:13, 10:13:46, all
`watchdog_reboot`, kernel silent), with no game running and nothing calling writeSysfs, but with `hidden_api_policy=1`
and the new OnnShell hook installed (reads only, and the heartbeat had stopped before the third). First idle reboot loop on
record. It did not repeat after the reboots at 10:27 or 11:12.

Reading:
- **The GPU cap does not prevent the watchdog death.** Mario at a hard 500 MHz (59% of stock clock, never exceeded on
  87 beats) died at 7:38, as long as the uncapped C0 (7:11) and inside the uncapped spread (54-455 s for NFS; Mario ~6 min).
- **Not heat.** M1 died at 51.2 C, and every death today was at 52 C or less, far below the 65 C throttle point. The idle
  boot loop after C0 is not explained by temperature either (47-52 C at those boots).
- **DDR stays at 1584 MHz** (DMC 792) in every beat, capped or not, idle or loaded: DDR frequency is fixed on this build,
  so the GPU clock is the only DVFS knob, and it does not help. `aml_ddr/bandwidth` needs `aml_ddr/mode` enabled
  (a write, not done).
- **The devfreq cap path is not clean on this kbase.** 666 MHz cannot be set (666666666 runs at 852; 666666000 puts the
  Amlogic platform code into idx:-1). Input events pass the cap through the power HAL preheat. NFS under a cap took a
  GPU_SHAREABILITY_FAULT twice (a3 at 666666000, C1 at 500000000), never seen in any earlier run or at stock clock; Mario
  under the same 500 MHz cap did not fault. Treat "lowered max_freq + NFS" as a new GPU fault trigger worth separating
  from the watchdog death; not chased further.
- Screenshots (`adb exec-out screencap`, every 30 s): they capture only OnnShell's overlay on black, not the zero-copy
  game frame, so they cannot show a frozen game.

Box at the end (11:14): max_freq 852000000 (read back; the cap does not survive a reboot), governor simple_ondemand,
`hidden_api_policy` deleted (null), no game running, default driver and `games/run_apk.sh` untouched
(md5 75de243e6a0f74a19d0ed0b5b3c6ca6b). Writes made during the series: devfreq max_freq, `hidden_api_policy`, and
the owner-approved `adb reboot`.

## Pattern vs traffic (Gaslighter, 2026-11-08)

Brief: the brief (private). Tool: `pb/memtest` (binary sha256 a8da1ffa, source `tools/memtest.c`), run through `pb/kald` (a renamed linker64) so OnnShell's GameSession ignores it. Panel on, 4K output, default driver 0ac69e67, each run launched at uptime >= 300 s. On-box heartbeat every 5 s (uptime, memtest pid, game pid, MemAvailable), soc_thermal every 30 s. Harness: a shell loop. Mario runs were unattended (title/demo loop).

| Run | Args | Outcome | MB/s seen | MemAvailable at launch | soc_thermal | Last heartbeat | File |
|---|---|---|---|---|---|---|---|
| A1 | memtest slow 1024 1000 | **survived 900 s** | 60-67 | 1,793,740 kB | 52.7 C | 12:02:21 (alive) | pat-A1.txt |
| A2 | memtest slow 1024 100 | **survived 900 s** | 590-597 | 1,935,340 kB | 52.7 C | 12:18:28 (alive) | pat-A2.txt |
| A3 | memtest slow 128 100 | not run (only if A2 dies) | | | | | |
| B0 | Mario, no shield | **survived 900 s**, 27,120 frames at a steady 30 fps | n/a | 2,025,668 kB | 52.7 C | 12:34:18 (alive) | pat-B0.txt |
| B1 | memtest shield 512, then Mario | **watchdog at ~6:41-6:57** | n/a | 1,894,424 kB (1.26 GB after shield) | 52.7 C | 12:41:40 | pat-B1.txt |
| B2 | memtest shield 1024, then Mario | **watchdog at ~13:39-14:02** (launched at uptime 303 s) | n/a | 1,647,252 kB (~0.78 GB after shield) | 51.5 C | 13:01:24 | pat-B2.txt |
| idle | after B2: box up from 13:02:34, our processes killed, HOME | **idle watchdog reboot about 1 min after boot** (SYSTEM_BOOT 13:04:29 "watchdog_reboot"); no memtest or game running | | | | | |

Reading:
- Part A: a 1 GB footprint walked at 65 MB/s and at ~600 MB/s survived 15 min each. A wide footprint at moderate rate is harmless. That leaves rate (memhog runs at 8.7 GB/s, about 15x A2), memhog's pattern (two 64 MB buffers copied back and forth), or simple chance.
- Part B: B0 (no shield) survived 15 min, while B1 and B2 died at 6.8 and 13.8 min. One sample each; B0's survival sits outside today's 6-7.6 min Mario band, so variance alone could explain it. No clear location effect: lifetimes did not swing with shield size in a consistent direction.
- The idle reboot about a minute after a fresh boot, with nothing of ours running, matches the brief's stop rule and ended the series. It is the first idle death recorded. Boot itself is a heavy DDR period (dex2oat, package scans, Play services), which fits "odds rise with load" as much as anything here.
- memtest was left running nowhere: the last two runs ended in reboots, and the idle check showed no memtest or kald process.

## Replacement unit (2026-11-08 evening)

Same firmware, same pb runtime and driver 0ac69e67. memhog 3 threads: survived 960 s at 8.43-8.48 GB/s (the old unit died at 50-340 s every time). Mario through ZeroCopyActivity: survived 16+ minutes, played by hand for part of it, still running when the watch ended. The old unit was defective; the watchdog deaths were never the driver, the runtime or the games.
