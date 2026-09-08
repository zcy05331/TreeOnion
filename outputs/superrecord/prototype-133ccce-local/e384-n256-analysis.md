# Generic super-record local comparison

Run class: `formal_candidate_selection`. 16 measured blocks, 3 warmups. Raw data: [e384-n256.json](e384-n256.json).

| Candidate | h / G / P; b; N0 | Mean ms | Median ms | p95 ms | SD ms | Estimated steady-state MiB |
|---|---:|---:|---:|---:|---:|---:|
| generic-h1-mandatory-0 | 1 / 256 / 1; 2; 8 | 32.072 | 32.044 | 32.450 | 0.252 | 12.70 |
| minimum-optimized-steady-state | 8 / 256 / 1; 2; 1 | 19.742 | 19.799 | 20.005 | 0.160 | 8.19 |
| minimum-response-count | 4 / 128 / 1; 0; 4 | 23.705 | 23.664 | 23.990 | 0.165 | 9.49 |
| generic-h1-frontier-1 | 1 / 128 / 1; 1; 8 | 32.574 | 32.569 | 32.903 | 0.196 | 12.12 |
| generic-h1-frontier-2 | 1 / 128 / 1; 2; 4 | 32.733 | 32.707 | 32.996 | 0.142 | 11.43 |
| frontier-1 | 8 / 256 / 1; 1; 2 | 19.053 | 19.002 | 19.631 | 0.212 | 8.27 |
| frontier-2 | 8 / 256 / 1; 0; 4 | 19.809 | 19.832 | 19.965 | 0.123 | 8.90 |

| Candidate | Control | Speedup | Paired 95% interval | Time reduction | Timing resolved |
|---|---|---:|---:|---:|---|
| minimum-optimized-steady-state | generic-h1-mandatory-0 | 1.625× | 1.617–1.632× | 38.45% | yes |
| minimum-response-count | generic-h1-mandatory-0 | 1.353× | 1.347–1.359× | 26.09% | yes |
| generic-h1-frontier-1 | generic-h1-mandatory-0 | 0.985× | 0.980–0.989× | -1.56% | yes |
| generic-h1-frontier-2 | generic-h1-mandatory-0 | 0.980× | 0.976–0.984× | -2.06% | yes |
| frontier-1 | generic-h1-mandatory-0 | 1.683× | 1.675–1.690× | 40.59% | yes |
| frontier-2 | generic-h1-mandatory-0 | 1.619× | 1.614–1.624× | 38.24% | yes |

Selected measured plan: **frontier-1**. smallest arithmetic mean; timing comparison resolved.

## Limitations

same leaf per shared measurement block; randomized candidate order within every block; OS drift remains. Only eligible measured candidates are ranked. Coefficient storage estimates are separate from measured process peak RSS. Query sizes are modeled; response sizes count serialized small-q ciphertext payload only.
