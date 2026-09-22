# Utah addendum: 2 GiB point, scan scratch reuse, pinned standard geometry

Host: zcy05331@er031.utah.cloudlab.us
Remote root: /users/zcy05331/tree-h1-addendum-20260921T2057Z
Campaign root it extends: /users/zcy05331/tree-h1-final-20260920T2105Z (complete)
Source: branch codex/addendum-2gib-scan-scratch at ae8ae664ca951172100c6adc97fef592b3709983

## What changed in the source

- select_level_m32 reuses one ScanScratch output buffer per response instead of
  allocating two fresh u64 buffers per level and plane (page faults were inside
  the scan_core timer).
- merkle_benchmarks accepts --reference-expansion-height; the report records
  reference_expansion_height / reference_fst_dim_sz / reference_other_dim_sz.
- Every runner admits height 25 (2 GiB); the core runner has --scales, --cases
  and --standard-reference-expansion-height.

## Queue (run-addendum.sh, phases in order)

1. ab_scratch: old campaign ep3 binary vs rebuilt ep3 binary, g8/N0 512/
   split-columns, 50 trials; 1 GiB alternating twice, 4 GiB once each.
   outputs/ab-scratch/.
2. primary_matrix: all five sizes re-measured with the rebuilt ep3 binary
   (pilots then two 50-trial layouts). outputs/primary/.
3. core_matrix_2gb: Standard, Layerwise tuned, Cuckoo tuned, Flat at 2 GiB.
   outputs/core-matrix/2gb/.
4. core_matrix_standard_h10: Standard at 256 and 512 MiB with expansion height
   10 (N0 = 512, the 1 GiB geometry). outputs/core-matrix-standard-h10/.
5. treepir_matrix: height 25 backend tuning + formal. outputs/treepir-backend-tuning/2gb/.
6. external_matrix: SPIRIT+RC and Respire at height 25. Respire's retained
   database is about 98 GiB; the runner admits it only if MemAvailable allows.

Shared, unchanged pieces are symlinked from the campaign root (hexl, SEAL,
SPIRIT, respire source, Rust toolchain, spirit and treepir_sealpir adapters).
TreePIR is a private copy re-patched for height 25 and rebuilt; the Respire
path_workload is rebuilt in this root's source/.benchmarks.

Layerwise and Cuckoo at 256/512 MiB still use the height-7 reference envelope;
only Standard was re-measured under the pinned geometry.
