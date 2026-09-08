# Generic super-record local comparison

Run class: `formal_candidate_selection`. 16 measured blocks, 3 warmups. Raw data: [e192-n256.json](e192-n256.json).

| Candidate | h / G / P; b; N0 | Mean ms | Median ms | p95 ms | SD ms | Estimated steady-state MiB |
|---|---:|---:|---:|---:|---:|---:|
| generic-h1-mandatory-0 | 1 / 256 / 1; 2; 8 | 31.966 | 32.026 | 32.395 | 0.237 | 12.70 |
| minimum-optimized-steady-state | 8 / 128 / 1; 1; 1 | 19.238 | 19.239 | 19.417 | 0.100 | 8.05 |
| minimum-response-count | 8 / 128 / 1; 0; 2 | 21.503 | 19.193 | 56.457 | 9.323 | 8.16 |
| generic-h1-frontier-1 | 1 / 128 / 1; 1; 8 | 32.562 | 32.567 | 33.111 | 0.241 | 12.12 |
| generic-h1-frontier-2 | 1 / 128 / 1; 2; 4 | 32.659 | 32.714 | 33.031 | 0.216 | 11.43 |
| frontier-1 | 8 / 256 / 1; 1; 2 | 19.107 | 19.142 | 19.418 | 0.204 | 8.27 |
| frontier-2 | 8 / 256 / 1; 2; 1 | 20.419 | 19.822 | 29.397 | 2.399 | 8.19 |

| Candidate | Control | Speedup | Paired 95% interval | Time reduction | Timing resolved |
|---|---|---:|---:|---:|---|
| minimum-optimized-steady-state | generic-h1-mandatory-0 | 1.662× | 1.655–1.669× | 39.82% | yes |
| minimum-response-count | generic-h1-mandatory-0 | 1.487× | 1.219–1.673× | 32.73% | yes |
| generic-h1-frontier-1 | generic-h1-mandatory-0 | 0.982× | 0.977–0.987× | -1.86% | yes |
| generic-h1-frontier-2 | generic-h1-mandatory-0 | 0.979× | 0.974–0.983× | -2.17% | yes |
| frontier-1 | generic-h1-mandatory-0 | 1.673× | 1.663–1.683× | 40.23% | yes |
| frontier-2 | generic-h1-mandatory-0 | 1.566× | 1.476–1.618× | 36.12% | yes |

Selected measured plan: **minimum-optimized-steady-state**. statistically unresolved or within 2%; storage tie-break applied; storage within 2%, response count tie-break applied.

## Limitations

same leaf per shared measurement block; randomized candidate order within every block; OS drift remains. Only eligible measured candidates are ranked. Coefficient storage estimates are separate from measured process peak RSS. Query sizes are modeled; response sizes count serialized small-q ciphertext payload only.
