# panvk-kbase on Mali-G310 (Amlogic S905X5M): four fixes + one note, validated up to a full game


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

005-008 are specific to running GLES-on-zink from a bundled arm64 bionic runtime; they are listed so the tree is reproducible, not as proposals.

## Also validated

- Toolchain to build for this target without a full NDK: distro clang-18 + NDK r27d **sysroot only** behind a thin shim (`--target=aarch64-linux-android29 --sysroot=<r27d> -resource-dir=... -rtlib=compiler-rt -fuse-ld=lld-18 -Qunused-arguments -unwindlib=libunwind`). The `-Qunused-arguments` is load-bearing: without it meson's `-Werror=unused-command-line-argument` + the shim's `-fuse-ld` fails every feature probe.
- Running a 64-bit ICD on a 32-bit-userspace box (no `/system/lib64`) via a bundled arm64 bionic from an Android-14 GSI runtime APEX. Out of scope for the driver, but it's how this was exercised; happy to share if useful to others on Amlogic boxes.
