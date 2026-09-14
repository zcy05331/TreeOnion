# Approximate gadget decomposition results

Final implementation: 8506e7f. Original control: a9503fb. All source work is on codex/generic-superrecord.

## Native 1 GiB, 32 B

Wisconsin sm110p-10s10616, Xeon Silver4314, CPU0, one NUMA node. Fixed n2048, t4099, q288135826036838401, q-prime4169729. Each point has two orders of50 measured queries and3 warmups per order:100 measured queries per point. All600 final measured queries returned the requested plaintexts correctly.

| Layout | Configuration | Server ms | MiB/s | Rotate ms | Project ms | R+P ms |
|---|---|---:|---:|---:|---:|---:|
| h3 | baseline | 936.908 | 1092.956 | 19.280 | 13.755 | 33.034 |
| h3 | kernel | 936.409 | 1093.539 | 18.929 | 14.136 | 33.065 |
| h3 | ep2-s36 | 814.375 | 1257.407 | 8.414 | 13.957 | 22.372 |
| h1 | baseline | 1129.215 | 906.824 | 87.778 | 64.350 | 152.128 |
| h1 | kernel | 1104.702 | 926.947 | 85.641 | 65.876 | 151.517 |
| h1 | ep2-s36 | 968.754 | 1057.028 | 36.648 | 65.734 | 102.382 |

h3 throughput advantage over h1: 20.53% before, 18.96% with the same EP2/drop36 configuration.

h3 layout is h3/G64/P1/N0=352/b9, expansion9. h1 layout is h1/G8/P3/N0=512/b7, expansion10. Both use the same leaf count and schedule. These are fixed-layout comparisons, not a new joint layout search.

The optimized data gadget uses2 rows, base2^12 and drops36 low ciphertext bits before signed decomposition. Matching gadget weights are2^48 and2^36. This rounds ciphertext decomposition, not database entries. Completion KEY stays10 exact rows; BV query-expansion/projection KS stays8 exact rows. The parameter defaults remain6/10/8; enable the measured point with PIR_EP_ROWS=2 and PIR_EP_LOW_BITS=36.

## Implementation and verification

- EP encryption, packed query gadget values and data decomposition use matching shifted weights. KEY decomposition remains independent.
- BV digit extraction is arranged across coefficients. Existing parameter defaults and plaintext semantics are preserved.
- GCC initially scalarized the GSW loops. The vectorization repair hoists immutable parameters and separates rounding, while avoiding signed-left-shift overflow. Native disassembly confirms vector centering, rounding and digit extraction; see local-validation/native-vector-fix.txt.
- Default regression:41/41 locally and on the final native build. Dense approximate GSW tests and12000 BV reconstruction cases passed. Benchmark-harness tests:11 passed.
- Final local confirmation:300 correct measured queries (three configurations), about256MiB under Rosetta; see formal-local-v2/. Initial screening and local/v1 native comparisons are retained separately.

## Communication and noise

All final rows use14880 B modeled seeded queries and11264 B actual response payloads. The existing helper-key upload model is unchanged for EP-only tuning. Noise diagnostics are retained; actual decryption, not a1-bit margin threshold, determines acceptance.
- h3 baseline: minimum budget 1 bits; 0/100 samples have zero diagnostic budget, all requested plaintexts correct.
- h3 kernel: minimum budget 1 bits; 0/100 samples have zero diagnostic budget, all requested plaintexts correct.
- h3 ep2-s36: minimum budget 0 bits; 4/100 samples have zero diagnostic budget, all requested plaintexts correct.
- h1 baseline: minimum budget 1 bits; 0/100 samples have zero diagnostic budget, all requested plaintexts correct.
- h1 kernel: minimum budget 1 bits; 0/100 samples have zero diagnostic budget, all requested plaintexts correct.
- h1 ep2-s36: minimum budget 0 bits; 83/100 samples have zero diagnostic budget, all requested plaintexts correct.

## Scope and rejected points

The initial EP3 exact, EP2/drop30, KS3 exact, and tested combined/KS2 settings failed complete-query screening; their logs remain in pilots/. Failure of a shared KS setting does not establish that short projection-only keys are impossible. Successful EP3/drop24, EP3/drop32 and EP2/drop36 points were tested locally. EP2/drop36 is the final native candidate.

Native-v1 timings include a diagnosed GCC regression and are historical diagnostics. Use native-v2/ for final claims. Projection itself remains about14ms for h3; the main reduction comes from folding, selector completion and rotation. Scan timings also vary between points despite unchanged matrix geometry; no hardware-counter ablation assigns those differences to the gadget change.

Approximate evaluation is implemented for logical K=1. Other entry-size and database-size points have not been remeasured with these new gadget parameters.

## Reproduction

Source knobs: -DACTIVE_CONFIG=CONFIG_N2048_K1_COMP -DPIR_EP_ROWS=2 -DPIR_EP_LOW_BITS=36 -DPIR_KS_ROWS=8 -DPIR_KS_LOW_BITS=0. Use the same native HEXL1.2.6 installation as the control.

scripts/benchmark_gadget_configs.py records commands, layout, source, per-stage samples and rejected configurations. run_native_v2.py is the retained finite native launcher. Remote root: /users/zcy05331/generic-gadgets-v2-20260909. All final metadata and outputs have been copied to native-v2/.
