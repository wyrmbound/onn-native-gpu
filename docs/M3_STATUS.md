# Path B status after M3 (2026-11-06)

## What works, reproducibly
- 64-bit PanVK (zenithblue-oss fork, g610-v10-csf) on the stock 32-bit onn 4K Pro, no root, no flash.
- M1 clear, M2 shader triangle, M3 triangle on the TV: `m3h` renders 1280x720, streams RGBA over
  loopback TCP 127.0.0.1:7331 to OnnShell's `display.FrameActivity`, which also launches `m3h`
  itself through Shizuku. No PC needed after Shizuku is up.

## Measured
| Path | Rate |
|---|---|
| GPU render + readback, 720p | 148 fps |
| End to end on screen (TCP + Java copy + hardware canvas), 720p | 13 fps |

The GPU is not the limit. The 3.7 MB per-frame CPU copy is.

## Hard limits (unchanged)
- No 64-bit Android runtime on the box: only custom arm64 code can use this driver; input, audio,
  window and lifecycle stay in the 32-bit app and cross a socket.
- Shizuku needs one manual start per boot (non-root). "No PC ever" = that one step.
- Direct display takeover is impossible on this user build (no DRM master for shell).

## B2 (zero-copy) if the owner says go
App allocates a gralloc buffer, hands its dma-buf fd to the renderer, renderer imports it via
kbase MEM_IMPORT UMM as a linear VkImage and renders straight into it; SurfaceFlinger shows it.
Expected cost: about M1 + M2 combined (fork bugs are the norm, three so far). Expected result:
display-rate rendering (60 fps) with the CPU out of the frame path. Only worth it if a real
payload exists that needs more than 13 fps.

## Also open
- Upstream report to zenithblue-oss: MEM_ALIAS refusal + desc-ring single map, trace buffer
  AUTO_VA bypass, panvk_image.c os_munmap-on-SAME_VA bug.
- Strip debug probes from the driver build.
