# Generic h1 optimization: first local exploration

Primary algorithm family: Generic h1. Reference implementation e83e3d7;
untiled prepared-query candidate f87da4c; tiled candidate806c73a. All changes
are isolated on `codex/generic-h1-opt-20260919`, based on refactored main plus
previously retained scan optimizations. The unfinished top-cache work is separate.

## Experiment scope

Apple M4/16GiB, macOS15.7.7, x86_64 Rosetta. Original logical tree256MiB,
32B entries, h1/G8/P3/N0=512/b5, expansion10, EP3/drop32, exactKS8, MLWE.
Each experiment uses reference→candidate then candidate→reference,16 measured
queries and3warmups per run. All128 measured complete-path queries across the
two experiments decoded correctly. No samples were removed. These are local
screening results, not native AVX512 or1GiB measurements.

## Results

| Experiment | Version | Prepare ms | Scan ms | Full server ms |
|---|---|---:|---:|---:|
| Direct split columns | reference | 38.815 | 315.400 | 533.079 |
| Direct split columns | candidate | 62.476 | 317.711 | 558.818 |
| Tiled split columns | reference | 43.627 | 349.484 | 587.242 |
| Tiled split columns | candidate | 19.062 | 353.188 | 560.124 |

Direct split columns regressed by4.80% and4.86% in the two rounds. Reject this
untiled implementation locally. Its scattered destination writes increased
preparation cost; tiling is the follow-up hypothesis tested here.

Tiled conversion reduces measured preparation by56.3% versus its control.
However, full-server results disagree by round: candidate is2.75% slower in
round0 and11.59% faster in round1. The pooled mean is4.62% lower, but this is
not sufficient evidence of a stable end-to-end improvement: run SDs are47/131ms
in round0 and72/26ms in round1, and the unmodified scan stage also drifts.
The local desktop had other active work. No specific source of the variation
has been isolated. Do not promote the pooled mean into a server speedup claim.

The new layout is built directly into existing-sized CRT buffers and reused
across planes/levels. It removes AVX512 query deinterleaving and temporary column
allocation in code; that instruction path was compiled but not executed locally.

Communication unchanged in every accepted run:14,880B modeled seeded query,
11,264B actual response ciphertext payload. Ciphertext parameters, helper-key
metadata, layouts and query schedules match within each A/B experiment.

## Verification

- DefaultEP6 reference:57/57 tests.
- Final tiled defaultEP6 candidate:57/57 and complete CLI/report regression pass.
- EP3 reference and first candidate each have the same9 failures in tests that
  assume default parameters;48/57 pass and no new failure was introduced. See
  `validation/ep3-regression-comparison.json`; these failures are not hidden as
  successful tests or silently relabeled not-applicable.
- Final EP3 alpha-layout equivalence, uint128 matrix oracle and MLWE E2E pass.
- Existing interleaved callers remain supported. New tests cover tiny/ragged
  widths,511/512,unsafe683/688/1024,32-bit-modulus fallback, and both CRT limbs.
- Independent review found no actionable correctness issue in the prepared-query
  change; tiled indexing was checked again against the equivalence tests.
- AVX512 translation unit compiled; native AVX512 execution remains untested.

## Decision and next discriminating experiment

Keep Generic h1/G8/P3/N0=512 as the measured algorithm reference. Keep the tiled
query-layout work as an experimental candidate; do not replace the validated
reference based on these noisy end-to-end local timings. Repeat the fixed-layout
A/B on the user's next native node before adoption. Both former CloudLab SSH
endpoints rejected login; user explicitly chose local-only testing for now.

Separately, `SELECTION-REVIEW.md` identifies the MLWE-blind cost model. Its best
archived h1 choice was nevertheless correct, so repairing the model does not
by itself establish a throughput gain. A new layout search must be evaluated
separately from this execution change. Avoid repeating the reverted CRT-Barrett
and candidate reserve/push_back experiments.

## Recompute

`python3 outputs/h1-optimization-20260919/summarize.py` validates retained raw
records and regenerates `summary.json`, including every round and stage.
