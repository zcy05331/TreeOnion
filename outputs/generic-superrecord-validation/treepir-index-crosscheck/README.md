# TreePIR Index Cross-Check Evidence

Date: 2026-09-04

Remote Java run directory:
`~/generic-superrecord-bench-20260904/deps/treepir-index-validation/run-20260904-crosscheck`

Inputs:
- `leaves_h24_0based.txt`, `leaves_h27_0based.txt`: 103-leaf local 0-based schedules from `make_leaf_schedule.py`.
- `list_TXs_24_2.txt`, `list_TXs_27_2.txt`: same schedules converted to Java `SubCSA` 1-based `TX_index` input.
- `SubCSA.java`: original public Java source uploaded to the remote validation directory.

Result:
- h24: `crosscheck_h24_summary.json` reports `pass: true`, `checked_triples: 2472`, `mismatch_count_reported: 0`.
- h27: `crosscheck_h27_summary.json` reports `pass: true`, `checked_triples: 2781`, `mismatch_count_reported: 0`.

Scope note:
This validates numeric `(leaf, color, node_id, index)` mapping against original Java `SubCSA`. It does not allocate or run the large TreePIR encrypted server benchmark.
