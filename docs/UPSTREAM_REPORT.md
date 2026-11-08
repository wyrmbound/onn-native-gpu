# panvk-kbase on Mali-G310 (Amlogic S905X5M): five fixes, one note and one platform fault, validated up to a full game



Target: zenithblue-oss/panvk-kbase-android (Mesa PanVK over mali_kbase), profile `g610-v10-csf`, Mesa base `5a07217f034`.

## Environment

- GPU: Mali-G310v2 MC1, `gpu_id 0xac740000` (Valhall, arch 10 / CSF). To our knowledge this is the **first arch-10 validation** of the `g610-v10-csf` profile on a real G310 — the profile was marked planned-not-validated and had only been exercised on G710/Pixel 7 (48-bit VA).
- SoC/board: Amlogic S905X5M ("onn 4K Pro" streaming box), Android 14.
- Kernel: 5.15.x GKI. Mali kbase **r44p1**, CSF user/kernel interface **1.20**.
- The issues below are latent upstream; they surface here because these paths are being exercised for the first time on this GPU/DDK (arch 10 + kbase CSF UK 1.20). The GPU VA is **at least 48-bit**: an EXEC_VA-zone BO sits at `0x800000001000` (= 2^47 + one page). kbase mmaps happen to land high (around `0x5fff_xxxxxxxx`) only because Linux places them top-down — that is not the VA ceiling.
- Validated end to end: `vkCmdClearColorImage` + readback (M1), a shader-drawn triangle through a full graphics pipeline + readback (M2, 64x64 and 1280x720), and the same streamed to a display (M3). On top of that, GLES 2.0 through zink on this PanVK (`GL renderer: zink Vulkan 1.4(Mali-G310v2 MC1 (MESA_PANVK))`, 59.9 fps at 1280x720), and a full game: **sm64ex runs 90 s+ at its native 29.9 fps at 1280x720 with audio**, presented zero-copy into Android HardwareBuffers. That game run is what exposed Finding 4.

---

## Finding 1 — CSF render descriptor ringbuffer: `KBASE_IOCTL_MEM_ALIAS` returns ENOMEM on this kbase (r44p1, UK 1.20)

**Symptom.** `init_render_desc_ringbuf()` (`panvk_vX_gpu_queue.c`) creates a 2x wrap-around mirror of the descriptor ring with `KBASE_IOCTL_MEM_ALIAS`. On this kernel the alias ioctl returns **ENOMEM** with >1 GB of GPU memory free, so device/queue creation fails before any submit. (The CS instruction ring already single-maps + pads and was fine; only the *descriptor* ring used the alias.)

**Root cause.** Not fully root-caused. `KBASE_IOCTL_MEM_ALIAS` returns ENOMEM on this kernel (kbase r44p1, CSF UK 1.20) for the 2× wrap-around mirror, with ample GPU memory free — so it is not a RAM shortage, but we did not isolate the DDK-specific reason the alias is refused. Rather than chase it, the fix removes the dependency on MEM_ALIAS entirely (below), which is a net simplification anyway.

**Fix.** Drop the MEM_ALIAS mirror on kbase. Over-allocate the ring BO and place the ring at a `size*2`-aligned offset inside the single mapping, so the emitter's existing 32-bit wrap arithmetic still has the alignment it relies on, and the spill area past the ring absorbs a chunk that straddles the end (the writer subtracts `size` on wrap). No second mapping, no alias.

```c
// init_render_desc_ringbuf(), kbase path
uint64_t bo_size = ringbuf->size;
#ifdef HAVE_PAN_KMOD_KBASE
if (gpu_queue_uses_kbase(dev))
   bo_size = ringbuf->size * 4;        // ring + spill + alignment slack
#endif
ringbuf->bo = pan_kmod_bo_alloc(dev->kmod.dev, dev->kmod.vm, bo_size, flags);
...
// map the whole BO (size = bo_size, AUTO_VA), then:
uint64_t base    = map_op.va.start;
uint64_t aligned = ALIGN_POT(base, (uint64_t)ringbuf->size * 2);
ringbuf->addr.dev = aligned;
if (ringbuf->addr.host)
   ringbuf->addr.host = (char *)ringbuf->addr.host + (aligned - base);
```

**Caveat worth upstreaming with it:** the single-mapped ring has no hardware wrap, so a descriptor write that crosses `ringbuf->size` is silent corruption instead of a wrap. A cheap `mesa_loge` + `VK_ERROR_DEVICE_LOST` guard in the desc-ring writer when a write would cross the end turns a future mystery into a one-line diagnosis. (Not yet implemented here.)

---

## Finding 2 — `panvk_image_plane_bind_mem()` frees a kbase SAME_VA BO with a raw `os_munmap` (latent on all kbase builds)

**Symptom.** Drawing into any CRC/AFBC-metadata-enabled image faults the GPU: `exception 0xC3` TRANSLATION_FAULT level 3, "Memory is not mapped on the GPU", at the tail of the image's own BO, *during the draw* — even though the BO was allocated and fully committed moments earlier. Reproduces deterministically; a 64x64 and a 1280x720 RGBA8 color attachment both hit it.

**Root cause.** In `panvk_image_plane_bind_mem()` the CRC zero-init path makes a *temporary* CPU mapping to clear the CRC state, then unmaps it with a **raw `os_munmap()`**:

```c
bool temporary_map = mem->addr.host == NULL;
uint8_t *cpu_map = temporary_map
   ? pan_kmod_bo_mmap(mem->bo, PROT_READ | PROT_WRITE, MAP_SHARED, NULL)
   : mem->addr.host;
...
if (temporary_map) {
   int ret = os_munmap(cpu_map, pan_kmod_bo_size(mem->bo));   // <-- bug
   assert(!ret);
}
```

On a DRM backend `pan_kmod_bo_mmap(..., NULL)` returns a *fresh* mapping that `os_munmap` can safely drop. On **kbase**, `kbase_kmod_bo_mmap` returns the BO's **single permanent cached mapping** (`cpu_ptr`), and for a SAME_VA region that CPU mapping *is* the region — `munmap(2)` of it triggers kbase's free-on-close and tears down the GPU mapping too. So the image BO's GPU PTEs are gone before the GPU ever executes the draw. The fault address is the clincher: it lands exactly on the sub-range of the freed region that the later allocations did *not* reuse (where no PTE remains), not on the overlapped part — which is what distinguishes a premature free from a mapping race. This is latent on every kbase build; it simply hadn't been exercised by a draw into a CRC-enabled image through this path.

**Fix (one line, correct on both backends).** Route the unmap through `pan_kmod_bo_munmap`, which `pan_kmod.h` already documents as the kbase-SAME_VA-safe wrapper (no-op when `host_addr == cpu_ptr`; falls back to `os_munmap` on backends without a `bo_munmap` op):

```diff
-         int ret = os_munmap(cpu_map, pan_kmod_bo_size(mem->bo));
+         /* kbase SAME_VA: pan_kmod_bo_mmap returned the BO's permanent cached
+          * mapping, not a fresh one, so a raw os_munmap() would tear down the
+          * GPU region (free-on-close). pan_kmod_bo_munmap is a no-op on kbase
+          * and a real munmap() on DRM backends. */
+         int ret = pan_kmod_bo_munmap(mem->bo, cpu_map, pan_kmod_bo_size(mem->bo));
          assert(!ret);
```

Audit: this is the only raw `os_munmap()` of a `pan_kmod_bo_mmap()` result under `src/panfrost` (the `panvk_host_copy.c` CRC/temp-map idiom already uses `pan_kmod_bo_munmap`). Worth a grep rule.

---

## Finding 3 — Note: CS trace buffer ignores AUTO_VA

Not a blocker; only under `PANVK_DEBUG=trace`. `init_subqueue_tracing()` maps the CS trace buffer at an explicit `priv_heap` GPU VA (a sub-4 GB address), bypassing the `PAN_KMOD_VM_FLAG_AUTO_VA` that the real allocators respect. On a kbase context that cannot honor a caller-chosen sub-4 GB VA (kbase's mmap rejects MAP_FIXED/hints; the FIXED_VA zone is high), the trace path fails to map. The non-trace path is AUTO_VA-clean, so this is cosmetic unless someone runs with trace on this class of GPU. Flagging in case the trace buffer should also go through AUTO_VA.

---

## Finding 4 — `g610-v10-csf` gets the kbase heap-renewal *policy* but not the tiler-work *accounting* (heavy workloads hang)

`csf/007-gpu-queue-kbase.patch` (applied to every CSF profile) adds the kbase tiler-heap renewal policy in `panvk_vX_gpu_queue.c`: with firmware chunk recycling disarmed on kbase, the heap only grows between renewals, and renewal fires after 128 tiler submits or once `kbase_tiler_work_count >= 65536`. Both triggers are gated on `submit->tiler_work_estimate`. The code that *writes* that estimate (per-draw accumulation in `panvk_cmd_draw` / `get_tiler_desc`, secondaries folded in `CmdExecuteCommands`, summed per submit in `panvk_queue_submit_init_storage`) lives only in `csf-v11/043-kbase-tiler-heap-renewal.patch`. `apply-patches.sh` gives a v10 profile the `csf` + `csf-v10` series, and `patches/csf-v10/` does not exist, so on v10 the estimate is always 0, renewal never runs, and the heap grows until it is exhausted.

Symptom on the G310 (sm64ex through zink): ~248 submits complete, then the vertex/tiler subqueue stalls inside its job's command-stream CALL with no fault (`kbase: timeout on subqueue 0 ... error 0x0`, stream progress 0), the fragment subqueue waits on it, and zink reports `DEVICE LOST`. 400 x 1 MiB chunks with zero recycling runs out right where a ~1.5 MiB-per-frame scene hangs. m1-m6 style tests never grow the heap enough to see it.

Fix used here: `patches/csf-v10/001-kbase-tiler-heap-renewal-writer.patch` = the `csf-v11/043` hunks verbatim, hunk headers regenerated against the v10 series (passes `apply-patches.sh`'s no-offset/no-fuzz check on a clean tree). Suggest upstream either gives v10 a `csf-v10` series that carries 043, or moves the accounting into `csf/007` next to the policy that depends on it.

Related: the v10 profile also misses `csf-v11/038-crc-init-kmod-munmap.patch`, which is the same bug as Finding 2 — the fix exists upstream for v11 but never reaches v10. Worth auditing which other `csf-v11` patches are arch-agnostic CSF/kbase fixes that v10 needs.

## Finding 5: cross-CSG waits between PanVK's per-subqueue groups wedge the GPU during CSG rotation, and the vendor GPU reset hard-locks the SoC

**Symptom.** Every 64-bit game on this box watchdog-reboots the whole SoC after a few minutes. The hardware watchdog fires (boot reason `watchdog`), there is no kernel log (pstore, devfreq and kbase sysfs are root-only, dropbox has no last kmsg), and userspace sees no `VK_ERROR_DEVICE_LOST`: the kernel dies first. Process RSS, threads and maps stay flat up to the end, so it is not a leak.

**Environment.** kbase r44p1, CSF UK 1.20, Amlogic S905X5M, Mali-G310 (arch 10). PanVK creates one kbase CSG per subqueue: 3 groups (VT, FRAG, COMPUTE), each with one CSI, per VkQueue, all at `BASE_QUEUE_GROUP_PRIORITY_HIGH`. The system has its own groups on the same GPU from the stock 32-bit Mali client (SurfaceFlinger, HWUI), which are active while the display is on.

**Evidence.** No kernel evidence is readable without root, so the evidence is differential. Workloads: Tyrian (a GL game through zink, presenting through our zero-copy path) and keepalive (a small Vulkan client that submits a clear-only render pass of a 64x64 image in a loop, no present).

| Run | Driver | Workload | Display | Our CSGs | Outcome |
|---|---|---|---|---|---|
| E1 | original baseline (001-008) | Tyrian, 2 s heartbeat | on | 3 active | box dead at 2:48 (28 fps), boot reason watchdog, RSS, threads and maps flat |
| E3 | original baseline (001-008) | Tyrian + keepalive on a second device (180 submits/s) | on | 3 active + 3 (keepalive, 1 active) | game fps halved; game hit a kbase subqueue 2 timeout and zink DEVICE LOST at 1:15; keepalive kept going erratically; box dead at about 6:50 |
| E0 | original baseline (001-008) | keepalive alone, 15 min | most likely off | 3, 1 active | survived, about 150k submits, no timeout |
| F1 | 001-012 | Tyrian | asleep for 3 min (6.5 fps), then woken | 3 active | died 4:30 after the display woke (watchdog), 9,900 frames, no userspace DEVICE LOST |
| X3 | 001-012 | keepalive alone, home screen animating | on | 3, 1 active | survived 900 s, 160,976 submits |
| G1 | 001-013 | Tyrian, home screen alive | on | 1 (3 CSIs) | alive at 12:45+, 20,820 frames at 27.3 fps, no DEVICE LOST |

keepalive is a plain Vulkan client on the same library, so before 013 its VkQueue also creates 3 groups. It only ever submits on one subqueue (a clear-only render pass, no draws), so it has no cross-group waits.

**Claim.** Three high-priority CSGs per queue plus cross-CSG waits wedge the GPU under rotation; one CSG per queue (013) fixed the 2D title. Three active CSGs with cross-CSG waits, display on, died at 2:48 to 6:50 (E1, E3, F1). Keepalive alone survived 15 min (E0), three groups with only one active survived 15 min (X3), and Tyrian on 013 with one CSG survived 27 min (G1). 013 does not fix sustained 3D titles; see Finding 6.

**Root cause (inferred from userspace only).** The trigger is cross-CSG synchronization during the firmware's group rotation: a CS in one of our groups waits on a sync object signalled by a sibling group while the firmware is suspending and resuming groups to share the CSG slots, including with the system's groups (SurfaceFlinger, HWUI). This wedges the GPU, and the vendor kernel's GPU reset then hard-locks the SoC instead of recovering, so the hardware watchdog reboots the box with no log and no DEVICE_LOST reaching userspace. We could not observe the firmware or the reset path directly.

**Fix.** `patches/csf-v10/013-kbase-one-csg-per-queue.patch`: one CSG per VkQueue with `cs_min = 3`, and the VT, FRAG and COMPUTE CS queues bound to it as CSI 0, 1 and 2. This makes the sibling syncs intra-group, which removes the trigger, and also cuts our CSG slot footprint from 3 to 1 per queue. Per-CSI USER_IO pages, doorbells and kicks were already per CS queue; teardown terminates the three CS queues, then the group. Group priority stays HIGH. G1 (Tyrian, a 2D title) is the first display-on game run that survived: 27 min where every earlier one died in 2:48 to 6:50. This fix covers the 2D title only; sustained 3D titles still die on 013 (Finding 6).

**Needed but not sufficient: csf-v11 077, 085 and 098.** Our csf-v10 009, 010 and 011 adapt them for v10 (SYSTEM scope for cross-subqueue sync signals, the desc ring and VkEvent syncs in CSF event memory, VERTEX_TILER_STARTED heap ops only on kbase). They were the right idea for cross-CSG waits and are kept, but they are not sufficient on this kbase: F1 and X3 ran with them (plus 012, fail fast on a group error), and F1 still died. Like 043 (Finding 4), they exist upstream for v11 but do not reach the v10 profile.

**HIGH priority does not help here.** The groups are created at `BASE_QUEUE_GROUP_PRIORITY_HIGH` with the comment "resist CSG rotation so the Pixel kbase off-slot heap-reclaim shrinker cannot empty our tiler heaps under memory pressure" (`kbase_kmod.c`). That is a workaround for the Pixel heap shrinker. On this kbase every failing run already had all three groups at HIGH; it did not prevent the rotation hang. A MEDIUM priority variant is staged (`csf-v10-staged/013v2`) but has not been run.

## Finding 6 (not a driver issue): sustained 3D titles hard-lock the SoC on 001-013, and so does a CPU memcpy loop

**Status: RESOLVED as a platform fault, not PanVK/kbase. Nothing to fix in the driver; documented so nobody chases it again.**

Same symptom as Finding 5 (watchdog reboot, no log, no DEVICE_LOST in userspace), with 013 applied, for 3D titles only. The rows below are what it looked like while we still thought it was ours.

| Run | Workload | Driver / renewal | Outcome |
|---|---|---|---|
| G2 | Super Mario 64 (3D, GL via zink) | 013, display on | died at 5:50 |
| N1 | NFS Most Wanted (3D, Vulkan direct, no zink) | 013, default renewal | died at about 3:00 |
| N4 | NFS | 013, renewal on every graphics submit | died at about 0:22 |
| N5 | NFS | 013 without 011 (stock heap ops), default renewal | died between 1:18 and 1:34 |
| N5b | NFS | 013 without 011, renewal off (only firmware chunk recycling) | frames advancing, died at about 2:30 |
| N3 | NFS | 013, renewal off | heap full at 0:44, group terminated cleanly, GPU idle, no reboot in 9 min |

N3 survived only because the GPU went idle after the group was terminated; it is not a survival of 3D load.

Reading: renewal frequency accelerates the death (N4 dies far sooner than N1), but sustained 3D rendering kills the SoC even without renewal (N5b). The 2D title on the same driver survives (G1, Finding 5).

**What it turned out to be (2026-11-07, about 25 watchdog reboots of A/B runs, all on NFS Most Wanted, full table in `docs/NFS_RUNS.md`).** None of the driver-side variables changed the outcome: heap renewal off or every submit, a pre-sized heap with zero heap ioctls during the run, stock heap ops (no 011), HEAP_SET only on change, 16x16 tiles, geometry pad, `PANVK_DEBUG=sync`, CSG priority MEDIUM, frame rate capped to 9 fps, a GPU keepalive alongside, skipping the game's movies. The display path was cleared too: composition is a DEVICE overlay, the HWUI read of our dma-bufs removed, even no picture posted at all. GPU clock is identical with the panel on and off (timed clear loop, ratio 1.00), and `soc_thermal` sat at 48-53 C. The kernel's own `SYSTEM_LAST_KMSG` for every watchdog boot is empty below the header: no mali line, no GPU fault, no soft lockup, no oops. The CPUs stop without printing.

Then the GPU was taken out of the picture: a 3-thread CPU `memcpy` loop over 64 MB buffers at about 8.7 GB/s, no game, no GPU work, panel on or off, reboots the box by watchdog on its own (three clean samples: 5.5 min, 55 s, and a 1-thread run that died within 10 s). NFS plus that loop dies in under 40 s, NFS alone in 1 to 7.5 min, the 2D title never. Time to death under the same configuration spans 54 to 455 s, so this is a random SoC lockup whose odds rise with DDR traffic, with no safe ceiling measurable from userspace. A 3D game is simply a heavy DDR client. 1080p output gave the longest NFS life (7.5 min) but still died; the game's own render scale at 33% did not help. A 65 W USB-C supply in place of the stock 10 W one changed nothing (CPU memcpy alone still died at about 4:50), so power delivery is ruled out.

Why this still belongs in the report: Finding 5's table and claim were built while this fault was active underneath, so some of its "died" rows (E1/E3/F1 with 3 groups) may carry a platform contribution. The one-CSG-per-queue argument stands on its own merits (cross-CSG waits during rotation are a real hazard on a 1-slot-starved firmware) and 013 did move Tyrian from minutes to 27+ minutes, but the clean before/after for it should be re-run on a board without this lockup before it is presented as proven.

**GPU clock cap tested (2026-11-08, through the vendor SystemControl HIDL service, which lets an ordinary app read and write `/sys/class/devfreq/fd000000.valhall/max_freq`).** Mario at a hard 500 MHz (59% of stock, never exceeded on 87 heartbeats) died by watchdog at 7:38, as long as uncapped NFS the same morning (7:11). DDR frequency reads a fixed 1584 MHz (DMC 792) in every sample, loaded or idle, so there is no memory DVFS to lower. Every death was at 52 C or below, far under the 65 C throttle point, so heat is ruled out too. Side finding: NFS under a lowered cap took a GPU_SHAREABILITY_FAULT (CSF fatal 0x00c00488, device lost, no reboot) twice, never seen at stock clock; Mario under the same cap did not fault. Also, after one death the box watchdog-rebooted twice more while idle, about a minute after each boot. Run table: capped-runs section of the NFS notes.

**For other people on Amlogic S905X5M boards:** if your 3D workloads reboot the box with an empty last_kmsg, run a CPU memcpy loop first. If that reboots it too, stop looking in Mesa; capping the GPU clock will not save you either.

---

## The patch set as carried here (`patches/csf-v10/`)

All apply in order after the stock `g610-v10-csf` series with `apply-patches.sh`'s strict no-offset/no-fuzz check. A clean base `5a07217f034` plus the series reproduces our working tree byte for byte.

| Patch | What | Upstream relevance |
|---|---|---|
| 001 kbase-tiler-heap-renewal-writer | csf-v11/043 rebased for v10 | **Finding 4** |
| 002 kbase-desc-ringbuf-single-map | no MEM_ALIAS desc ring | **Finding 1** |
| 003 kbase-fixed-va-bo-replace | `vm_bind` at an explicit VA via `MEM_ALLOC_EX` + `BASE_MEM_FIXED` (CSF UK >= 1.9) instead of failing | related to Finding 3; carried in the working tree but **not exercised** (the trace path was not re-run with it), so listed for reproducibility rather than proposed |
| 004 kbase-image-temp-map-munmap | `pan_kmod_bo_munmap` for the CRC temp map | **Finding 2** (same as csf-v11/038) |
| 005 zink-direct-icd-load | zink dlopens the ICD via `ZINK_VK_ICD` when there is no Vulkan loader | our runtime only (bionic, no loader) |
| 006 dri-kopper-on-android | build real `kopper.c` for zink on Android | our GL path only |
| 007 egl-surfaceless-zink-fallback | surfaceless takes the no-DRM probe for zink too | our GL path only (DRM node is the display controller) |
| 008 android-log-stderr-sink | Mesa logs also to stderr on Android | dev convenience; with the bundled GSI liblog, Mesa's logcat sink produced no output at all, which hid every finding above until this patch |
| 009 kbase-system-scope-subqueue-sync-signals | csf-v11/077 adapted for v10: SYSTEM scope for cross-subqueue sync signals | needed but not sufficient for **Finding 5** |
| 010 kbase-desc-ring-and-vkevent-syncs-in-csf-event-memory | csf-v11/085 adapted for v10: desc ring and VkEvent syncs in CSF event memory | needed but not sufficient for **Finding 5** |
| 011 kbase-vertex-tiler-started-heap-ops-only | csf-v11/098 verbatim, headers regenerated for v10 | needed but not sufficient for **Finding 5** |
| 012 kbase-wait-fail-fast-on-group-error | check the latched CSF group error in the completion wait | diagnostic |
| 013 kbase-one-csg-per-queue | one CSG with 3 CSIs per VkQueue instead of one CSG per subqueue | **Finding 5** |

005-008 are specific to running GLES-on-zink from a bundled arm64 bionic runtime; they are listed so the tree is reproducible, not as proposals.

## Also validated

- Toolchain to build for this target without a full NDK: distro clang-18 + NDK r27d **sysroot only** behind a thin shim (`--target=aarch64-linux-android29 --sysroot=<r27d> -resource-dir=... -rtlib=compiler-rt -fuse-ld=lld-18 -Qunused-arguments -unwindlib=libunwind`). The `-Qunused-arguments` is load-bearing: without it meson's `-Werror=unused-command-line-argument` + the shim's `-fuse-ld` fails every feature probe.
- Running a 64-bit ICD on a 32-bit-userspace box (no `/system/lib64`) via a bundled arm64 bionic from an Android-14 GSI runtime APEX. Out of scope for the driver, but it's how this was exercised; happy to share if useful to others on Amlogic boxes.
