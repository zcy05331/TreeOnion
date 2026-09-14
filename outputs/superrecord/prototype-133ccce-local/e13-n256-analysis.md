# Generic super-record local comparison

Run class: `formal_candidate_selection`. 16 measured blocks, 3 warmups. Raw data: [e13-n256.json](e13-n256.json).

| Candidate | h / G / P; b; N0 | Mean ms | Median ms | p95 ms | SD ms | Estimated steady-state MiB |
|---|---:|---:|---:|---:|---:|---:|
| generic-h1-mandatory-0 | 1 / 256 / 1; 2; 8 | 31.883 | 31.850 | 32.646 | 0.281 | 12.70 |
| minimum-optimized-steady-state | 8 / 16 / 1; 0; 1 | 19.297 | 19.283 | 19.659 | 0.196 | 8.01 |
| minimum-response-count | 8 / 128 / 1; 0; 2 | 19.011 | 19.049 | 19.560 | 0.286 | 8.16 |
| generic-h1-frontier-1 | 1 / 128 / 1; 1; 8 | 32.406 | 32.412 | 33.005 | 0.389 | 12.12 |
| generic-h1-frontier-2 | 1 / 256 / 1; 1; 16 | 32.651 | 32.649 | 33.043 | 0.284 | 14.20 |
| frontier-1 | 8 / 256 / 1; 1; 2 | 19.010 | 18.960 | 19.405 | 0.229 | 8.27 |
| frontier-2 | 8 / 128 / 1; 1; 1 | 19.148 | 19.153 | 19.652 | 0.207 | 8.05 |

| Candidate | Control | Speedup | Paired 95% interval | Time reduction | Timing resolved |
|---|---|---:|---:|---:|---|
| minimum-optimized-steady-state | generic-h1-mandatory-0 | 1.652× | 1.642–1.663× | 39.48% | yes |
| minimum-response-count | generic-h1-mandatory-0 | 1.677× | 1.666–1.689× | 40.37% | yes |
| generic-h1-frontier-1 | generic-h1-mandatory-0 | 0.984× | 0.978–0.990× | -1.64% | yes |
| generic-h1-frontier-2 | generic-h1-mandatory-0 | 0.976× | 0.971–0.982× | -2.41% | yes |
| frontier-1 | generic-h1-mandatory-0 | 1.677× | 1.669–1.686× | 40.38% | yes |
| frontier-2 | generic-h1-mandatory-0 | 1.665× | 1.659–1.672× | 39.94% | yes |

Selected measured plan: **minimum-response-count**. statistically unresolved or within 2%; storage tie-break applied; storage within 2%, response count tie-break applied.

## Limitations

same leaf per shared measurement block; randomized candidate order within every block; OS drift remains. Only eligible measured candidates are ranked. Coefficient storage estimates are separate from measured process peak RSS. Query sizes are modeled; response sizes count serialized small-q ciphertext payload only.
