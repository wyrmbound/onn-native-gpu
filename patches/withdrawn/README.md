# Withdrawn patches

Kept for the record. These are not part of any applied series and no script uses them.

| Patch | Was | Status |
|---|---|---|
| `013-kbase-one-csg-per-queue.patch` | csf-v10 013: one kbase CSG with three CSIs per VkQueue instead of one CSG per subqueue | Withdrawn 2026-11-09: on a healthy unit Tyrian (SDL3 + GL via zink) hit GPU_SHAREABILITY_FAULT (CSF exception 0x88) and survived 0 of 3 runs on 013, and survived 3 of 4 on 001-012. 013 is not shown to cause the fault; 001-012 is the safer default. See Finding 7 in `docs/UPSTREAM_REPORT.md`. |
