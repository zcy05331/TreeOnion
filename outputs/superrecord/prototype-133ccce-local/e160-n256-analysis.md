# Generic super-record local comparison

Run class: `formal_candidate_selection`. 16 measured blocks, 3 warmups. Raw data: [e160-n256.json](e160-n256.json).

| Candidate | h / G / P; b; N0 | Mean ms | Median ms | p95 ms | SD ms | Estimated steady-state MiB |
|---|---:|---:|---:|---:|---:|---:|
| generic-h1-mandatory-0 | 1 / 256 / 1; 2; 8 | 31.972 | 31.951 | 32.511 | 0.217 | 12.70 |
| minimum-optimized-steady-state | 8 / 128 / 1; 1; 1 | 19.175 | 19.191 | 19.370 | 0.093 | 8.05 |
| minimum-response-count | 8 / 128 / 1; 0; 2 | 19.121 | 19.128 | 19.299 | 0.115 | 8.16 |
| generic-h1-frontier-1 | 1 / 128 / 1; 1; 8 | 32.412 | 32.361 | 32.701 | 0.204 | 12.12 |
| generic-h1-frontier-2 | 1 / 256 / 1; 1; 16 | 32.743 | 32.688 | 33.082 | 0.194 | 14.20 |
| frontier-1 | 8 / 256 / 1; 1; 2 | 19.058 | 19.066 | 19.302 | 0.133 | 8.27 |
| frontier-2 | 8 / 256 / 1; 2; 1 | 19.704 | 19.708 | 20.092 | 0.159 | 8.19 |

| Candidate | Control | Speedup | Paired 95% interval | Time reduction | Timing resolved |
|---|---|---:|---:|---:|---|
| minimum-optimized-steady-state | generic-h1-mandatory-0 | 1.667× | 1.662–1.673× | 40.03% | yes |
| minimum-response-count | generic-h1-mandatory-0 | 1.672× | 1.665–1.679× | 40.19% | yes |
| generic-h1-frontier-1 | generic-h1-mandatory-0 | 0.986× | 0.982–0.990× | -1.38% | yes |
| generic-h1-frontier-2 | generic-h1-mandatory-0 | 0.976× | 0.972–0.980× | -2.41% | yes |
| frontier-1 | generic-h1-mandatory-0 | 1.678× | 1.670–1.685× | 40.39% | yes |
| frontier-2 | generic-h1-mandatory-0 | 1.623× | 1.614–1.631× | 38.37% | yes |

Selected measured plan: **minimum-response-count**. statistically unresolved or within 2%; storage tie-break applied; storage within 2%, response count tie-break applied.

## Limitations

same leaf per shared measurement block; randomized candidate order within every block; OS drift remains. Only eligible measured candidates are ranked. Coefficient storage estimates are separate from measured process peak RSS. Query sizes are modeled; response sizes count serialized small-q ciphertext payload only.
