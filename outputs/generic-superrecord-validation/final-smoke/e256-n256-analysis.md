# Generic super-record local comparison

Run class: `smoke_only`. 2 measured blocks, 1 warmups. Raw data: [e256-n256.json](e256-n256.json).

| Candidate | h / G / P; b; N0 | Mean ms | Median ms | p95 ms | SD ms | Estimated steady-state MiB |
|---|---:|---:|---:|---:|---:|---:|
| bundle3 | 3 / 64 / 1; 1; 2 | 28.987 | 28.987 | 29.074 | 0.123 | 9.87 |
| generic-h1-mandatory-1 | 1 / 128 / 1; 1; 8 | 33.150 | 33.150 | 33.251 | 0.144 | 12.12 |
| minimum-optimized-steady-state | 8 / 64 / 3; 0; 1 | 30.909 | 30.909 | 30.941 | 0.046 | 8.35 |
| minimum-response-count | 6 / 128 / 1; 0; 3 | 24.429 | 24.429 | 24.638 | 0.295 | 9.40 |
| generic-h1-frontier-1 | 1 / 128 / 1; 2; 4 | 33.313 | 33.313 | 33.383 | 0.100 | 11.43 |
| generic-h1-frontier-2 | 1 / 64 / 1; 1; 4 | 33.945 | 33.945 | 33.985 | 0.057 | 11.13 |
| frontier-1 | 4 / 128 / 1; 0; 4 | 24.328 | 24.328 | 24.558 | 0.324 | 9.49 |
| legacy-g32 | — / — / —; —; — | 29.316 | 29.316 | 29.538 | 0.314 | unavailable |

| Candidate | Control | Speedup | Paired 95% interval | Time reduction | Timing resolved |
|---|---|---:|---:|---:|---|
| bundle3 | legacy-g32 | 1.011× | 1.007–1.016× | 1.12% | yes |
| bundle3 | generic-h1-mandatory-1 | 1.144× | 1.137–1.151× | 12.56% | yes |
| generic-h1-mandatory-1 | legacy-g32 | 0.884× | 0.875–0.894× | -13.08% | yes |
| minimum-optimized-steady-state | legacy-g32 | 0.948× | 0.940–0.957× | -5.43% | yes |
| minimum-optimized-steady-state | generic-h1-mandatory-1 | 1.072× | 1.070–1.075× | 6.76% | yes |
| minimum-response-count | legacy-g32 | 1.200× | 1.199–1.201× | 16.67% | yes |
| minimum-response-count | generic-h1-mandatory-1 | 1.357× | 1.341–1.373× | 26.31% | yes |
| generic-h1-frontier-1 | legacy-g32 | 0.880× | 0.872–0.889× | -13.63% | yes |
| generic-h1-frontier-1 | generic-h1-mandatory-1 | 0.995× | 0.994–0.996× | -0.49% | yes |
| generic-h1-frontier-2 | legacy-g32 | 0.864× | 0.858–0.869× | -15.79% | yes |
| generic-h1-frontier-2 | generic-h1-mandatory-1 | 0.977× | 0.972–0.981× | -2.40% | yes |
| frontier-1 | legacy-g32 | 1.205× | 1.203–1.207× | 17.01% | yes |
| frontier-1 | generic-h1-mandatory-1 | 1.363× | 1.346–1.380× | 26.61% | yes |

Selected measured plan: **frontier-1**. statistically unresolved or within 2%; storage tie-break applied; storage within 2%, response count tie-break applied.

## Limitations

same leaf per shared measurement block; randomized candidate order within every block; OS drift remains. Only eligible measured candidates are ranked. Coefficient storage estimates are separate from measured process peak RSS. Query sizes are modeled; response sizes count serialized small-q ciphertext payload only.
