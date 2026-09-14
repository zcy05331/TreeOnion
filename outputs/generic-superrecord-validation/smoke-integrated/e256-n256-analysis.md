# Generic super-record local comparison

Run class: `smoke_only`. 2 measured blocks, 1 warmups. Raw data: [e256-n256.json](e256-n256.json).

| Candidate | h / G / P | Mean ms | Median ms | p95 ms | SD ms | Estimated steady-state MiB |
|---|---:|---:|---:|---:|---:|---:|
| bundle3 | 3 / 64 / 1 | 27.510 | 27.510 | 27.623 | 0.159 | 9.87 |
| generic-h1-mandatory-1 | 1 / 128 / 1 | 31.545 | 31.545 | 31.586 | 0.058 | 12.12 |
| minimum-optimized-steady-state | 8 / 64 / 3 | 29.671 | 29.671 | 29.707 | 0.050 | 8.35 |
| minimum-response-count | 6 / 128 / 1 | 23.046 | 23.046 | 23.068 | 0.030 | 9.40 |
| generic-h1-frontier-1 | 1 / 128 / 1 | 31.767 | 31.767 | 31.834 | 0.095 | 11.43 |
| generic-h1-frontier-2 | 1 / 64 / 1 | 32.230 | 32.230 | 32.277 | 0.067 | 11.13 |
| frontier-1 | 4 / 128 / 1 | 22.978 | 22.978 | 22.999 | 0.029 | 9.49 |
| legacy-g32 | — / — / — | 27.824 | 27.824 | 28.018 | 0.274 | unavailable |

| Candidate | Control | Speedup | Paired 95% interval | Time reduction | Timing resolved |
|---|---|---:|---:|---:|---|
| bundle3 | legacy-g32 | 1.011× | 1.000–1.023× | 1.13% | yes |
| bundle3 | generic-h1-mandatory-1 | 1.147× | 1.143–1.150× | 12.79% | yes |
| generic-h1-mandatory-1 | legacy-g32 | 0.882× | 0.875–0.889× | -13.38% | yes |
| minimum-optimized-steady-state | legacy-g32 | 0.938× | 0.932–0.943× | -6.64% | yes |
| minimum-optimized-steady-state | generic-h1-mandatory-1 | 1.063× | 1.061–1.066× | 5.94% | yes |
| minimum-response-count | legacy-g32 | 1.207× | 1.200–1.215× | 17.17% | yes |
| minimum-response-count | generic-h1-mandatory-1 | 1.369× | 1.366–1.372× | 26.94% | yes |
| generic-h1-frontier-1 | legacy-g32 | 0.876× | 0.868–0.884× | -14.17% | yes |
| generic-h1-frontier-1 | generic-h1-mandatory-1 | 0.993× | 0.992–0.994× | -0.70% | yes |
| generic-h1-frontier-2 | legacy-g32 | 0.863× | 0.856–0.871× | -15.84% | yes |
| generic-h1-frontier-2 | generic-h1-mandatory-1 | 0.979× | 0.979–0.979× | -2.17% | yes |
| frontier-1 | legacy-g32 | 1.211× | 1.203–1.218× | 17.41% | yes |
| frontier-1 | generic-h1-mandatory-1 | 1.373× | 1.370–1.376× | 27.16% | yes |

Selected measured plan: **frontier-1**. statistically unresolved or within 2%; storage tie-break applied; storage within 2%, response count tie-break applied.

## Limitations

same recorded leaf schedule and randomized candidate batches; OS drift remains. Only eligible measured candidates are ranked. Coefficient storage estimates are separate from measured process peak RSS. Query sizes are modeled; response sizes count serialized small-q ciphertext payload only.
