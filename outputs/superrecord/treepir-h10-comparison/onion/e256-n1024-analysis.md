# Generic super-record local comparison

Run class: `formal_candidate_selection`. 16 measured blocks, 3 warmups. Raw data: [e256-n1024.json](e256-n1024.json).

| Candidate | h / G / P; b; N0 | Mean ms | Median ms | p95 ms | SD ms | Estimated steady-state MiB |
|---|---:|---:|---:|---:|---:|---:|
| bundle3 | 3 / 64 / 1; 1; 6 | 37.305 | 37.317 | 38.007 | 0.363 | 12.45 |
| generic-h1-mandatory-1 | 1 / 128 / 1; 2; 16 | 42.623 | 42.414 | 44.725 | 0.744 | 16.94 |
| minimum-optimized-steady-state | 4 / 128 / 1; 3; 2 | 33.632 | 33.540 | 35.464 | 0.511 | 11.24 |
| minimum-response-count | 6 / 128 / 1; 1; 6 | 32.282 | 31.915 | 37.499 | 1.418 | 11.76 |
| generic-h1-frontier-1 | 1 / 128 / 1; 3; 8 | 43.052 | 42.698 | 46.744 | 1.076 | 15.50 |
| generic-h1-frontier-2 | 1 / 64 / 1; 2; 8 | 42.891 | 42.936 | 43.350 | 0.282 | 14.39 |
| frontier-1 | 5 / 128 / 1; 1; 7 | 31.906 | 31.880 | 32.283 | 0.200 | 12.03 |
| frontier-2 | 4 / 128 / 1; 1; 8 | 32.105 | 32.059 | 32.477 | 0.220 | 12.15 |
| legacy-g32 | — / — / —; —; — | 34.347 | 34.333 | 34.899 | 0.221 | unavailable |

| Candidate | Control | Speedup | Paired 95% interval | Time reduction | Timing resolved |
|---|---|---:|---:|---:|---|
| bundle3 | legacy-g32 | 0.921× | 0.916–0.926× | -8.61% | yes |
| bundle3 | generic-h1-mandatory-1 | 1.143× | 1.133–1.153× | 12.48% | yes |
| generic-h1-mandatory-1 | legacy-g32 | 0.806× | 0.800–0.811× | -24.10% | yes |
| minimum-optimized-steady-state | legacy-g32 | 1.021× | 1.015–1.026× | 2.08% | yes |
| minimum-optimized-steady-state | generic-h1-mandatory-1 | 1.267× | 1.261–1.274× | 21.09% | yes |
| minimum-response-count | legacy-g32 | 1.064× | 1.041–1.078× | 6.01% | yes |
| minimum-response-count | generic-h1-mandatory-1 | 1.320× | 1.297–1.338× | 24.26% | yes |
| generic-h1-frontier-1 | legacy-g32 | 0.798× | 0.787–0.806× | -25.35% | yes |
| generic-h1-frontier-1 | generic-h1-mandatory-1 | 0.990× | 0.975–1.003× | -1.01% | no |
| generic-h1-frontier-2 | legacy-g32 | 0.801× | 0.797–0.805× | -24.88% | yes |
| generic-h1-frontier-2 | generic-h1-mandatory-1 | 0.994× | 0.985–1.004× | -0.63% | no |
| frontier-1 | legacy-g32 | 1.076× | 1.072–1.081× | 7.11% | yes |
| frontier-1 | generic-h1-mandatory-1 | 1.336× | 1.325–1.349× | 25.14% | yes |
| frontier-2 | legacy-g32 | 1.070× | 1.065–1.075× | 6.53% | yes |
| frontier-2 | generic-h1-mandatory-1 | 1.328× | 1.318–1.339× | 24.68% | yes |

Selected measured plan: **minimum-response-count**. statistically unresolved or within 2%; storage tie-break applied.

## Limitations

same leaf per shared measurement block; randomized candidate order within every block; OS drift remains. Only eligible measured candidates are ranked. Coefficient storage estimates are separate from measured process peak RSS. Query sizes are modeled; response sizes count serialized small-q ciphertext payload only.
