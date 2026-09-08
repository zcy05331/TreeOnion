# Generic super-record local comparison

Run class: `formal_candidate_selection`. 16 measured blocks, 3 warmups. Raw data: [e768-n256.json](e768-n256.json).

| Candidate | h / G / P; b; N0 | Mean ms | Median ms | p95 ms | SD ms | Estimated steady-state MiB |
|---|---:|---:|---:|---:|---:|---:|
| generic-h1-mandatory-0 | 1 / 256 / 1; 2; 8 | 31.473 | 31.449 | 31.864 | 0.209 | 12.70 |
| minimum-optimized-steady-state | 4 / 256 / 1; 3; 1 | 25.250 | 25.237 | 25.545 | 0.161 | 9.37 |
| minimum-response-count | 2 / 128 / 1; 1; 4 | 27.424 | 27.433 | 27.825 | 0.195 | 10.38 |
| generic-h1-frontier-1 | 1 / 128 / 1; 1; 8 | 32.256 | 32.252 | 32.527 | 0.166 | 12.12 |
| generic-h1-frontier-2 | 1 / 256 / 1; 1; 16 | 32.155 | 32.163 | 32.653 | 0.232 | 14.20 |
| frontier-1 | 4 / 256 / 1; 1; 4 | 22.990 | 22.974 | 23.217 | 0.126 | 9.67 |
| frontier-2 | 4 / 256 / 1; 2; 2 | 23.504 | 23.513 | 23.829 | 0.178 | 9.38 |

| Candidate | Control | Speedup | Paired 95% interval | Time reduction | Timing resolved |
|---|---|---:|---:|---:|---|
| minimum-optimized-steady-state | generic-h1-mandatory-0 | 1.246× | 1.241–1.251× | 19.77% | yes |
| minimum-response-count | generic-h1-mandatory-0 | 1.148× | 1.144–1.151× | 12.86% | yes |
| generic-h1-frontier-1 | generic-h1-mandatory-0 | 0.976× | 0.972–0.980× | -2.49% | yes |
| generic-h1-frontier-2 | generic-h1-mandatory-0 | 0.979× | 0.975–0.982× | -2.17% | yes |
| frontier-1 | generic-h1-mandatory-0 | 1.369× | 1.363–1.375× | 26.95% | yes |
| frontier-2 | generic-h1-mandatory-0 | 1.339× | 1.333–1.345× | 25.32% | yes |

Selected measured plan: **frontier-1**. smallest arithmetic mean; timing comparison resolved.

## Limitations

same leaf per shared measurement block; randomized candidate order within every block; OS drift remains. Only eligible measured candidates are ranked. Coefficient storage estimates are separate from measured process peak RSS. Query sizes are modeled; response sizes count serialized small-q ciphertext payload only.
