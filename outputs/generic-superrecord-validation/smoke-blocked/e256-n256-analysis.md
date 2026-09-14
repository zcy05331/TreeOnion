# Generic super-record local comparison

Run class: `smoke_only`. 2 measured blocks, 1 warmups. Raw data: [e256-n256.json](e256-n256.json).

| Candidate | h / G / P | Mean ms | Median ms | p95 ms | SD ms | Estimated steady-state MiB |
|---|---:|---:|---:|---:|---:|---:|
| bundle3 | 3 / 64 / 1 | 27.714 | 27.714 | 27.800 | 0.120 | 9.87 |
| generic-h1-mandatory-1 | 1 / 128 / 1 | 31.491 | 31.491 | 31.647 | 0.220 | 12.12 |
| minimum-optimized-steady-state | 8 / 64 / 3 | 29.535 | 29.535 | 29.568 | 0.047 | 8.35 |
| minimum-response-count | 6 / 128 / 1 | 22.804 | 22.804 | 22.845 | 0.057 | 9.40 |
| generic-h1-frontier-1 | 1 / 128 / 1 | 31.739 | 31.739 | 31.841 | 0.144 | 11.43 |
| generic-h1-frontier-2 | 1 / 64 / 1 | 32.497 | 32.497 | 32.562 | 0.092 | 11.13 |
| frontier-1 | 4 / 128 / 1 | 23.133 | 23.133 | 23.197 | 0.090 | 9.49 |
| legacy-g32 | — / — / — | 28.036 | 28.036 | 28.458 | 0.597 | unavailable |

| Candidate | Control | Speedup | Paired 95% interval | Time reduction | Timing resolved |
|---|---|---:|---:|---:|---|
| bundle3 | legacy-g32 | 1.012× | 0.999–1.024× | 1.15% | no |
| bundle3 | generic-h1-mandatory-1 | 1.136× | 1.127–1.145× | 11.99% | yes |
| generic-h1-mandatory-1 | legacy-g32 | 0.890× | 0.873–0.908× | -12.32% | yes |
| minimum-optimized-steady-state | legacy-g32 | 0.949× | 0.936–0.962× | -5.35% | yes |
| minimum-optimized-steady-state | generic-h1-mandatory-1 | 1.066× | 1.060–1.073× | 6.21% | yes |
| minimum-response-count | legacy-g32 | 1.229× | 1.213–1.246× | 18.66% | yes |
| minimum-response-count | generic-h1-mandatory-1 | 1.381× | 1.372–1.390× | 27.58% | yes |
| generic-h1-frontier-1 | legacy-g32 | 0.883× | 0.873–0.894× | -13.21% | yes |
| generic-h1-frontier-1 | generic-h1-mandatory-1 | 0.992× | 0.984–1.000× | -0.79% | no |
| generic-h1-frontier-2 | legacy-g32 | 0.863× | 0.848–0.877× | -15.91% | yes |
| generic-h1-frontier-2 | generic-h1-mandatory-1 | 0.969× | 0.966–0.972× | -3.19% | yes |
| frontier-1 | legacy-g32 | 1.212× | 1.190–1.234× | 17.49% | yes |
| frontier-1 | generic-h1-mandatory-1 | 1.361× | 1.358–1.364× | 26.54% | yes |

Selected measured plan: **minimum-response-count**. statistically unresolved or within 2%; storage tie-break applied; storage within 2%, response count tie-break applied.

## Limitations

same leaf per shared measurement block; randomized candidate order within every block; OS drift remains. Only eligible measured candidates are ranked. Coefficient storage estimates are separate from measured process peak RSS. Query sizes are modeled; response sizes count serialized small-q ciphertext payload only.
