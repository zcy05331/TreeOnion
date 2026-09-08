# Generic super-record local comparison

Run class: `formal_candidate_selection`. 16 measured blocks, 3 warmups. Raw data: [e256-n256.json](e256-n256.json).

| Candidate | h / G / P | Mean ms | Median ms | p95 ms | SD ms | Estimated steady-state MiB |
|---|---:|---:|---:|---:|---:|---:|
| bundle3 | 3 / 64 / 1 | 28.488 | 28.315 | 29.917 | 0.495 | 9.87 |
| generic-h1-mandatory-1 | 1 / 128 / 1 | 32.302 | 32.168 | 32.845 | 0.338 | 11.43 |
| minimum-optimized-steady-state | 8 / 64 / 3 | 30.157 | 30.152 | 30.398 | 0.170 | 8.35 |
| minimum-response-count | 4 / 128 / 1 | 23.517 | 23.440 | 24.049 | 0.241 | 9.18 |
| generic-h1-frontier-1 | 1 / 128 / 1 | 32.416 | 32.367 | 32.876 | 0.268 | 12.12 |
| generic-h1-frontier-2 | 1 / 64 / 1 | 32.741 | 32.781 | 33.051 | 0.197 | 11.13 |
| frontier-1 | 6 / 128 / 1 | 23.456 | 23.439 | 23.784 | 0.172 | 9.40 |
| frontier-2 | 4 / 128 / 1 | 23.610 | 23.526 | 24.290 | 0.238 | 9.49 |
| legacy-g32 | — / — / — | 28.895 | 28.892 | 29.846 | 0.399 | unavailable |

| Candidate | Control | Speedup | Paired 95% interval | Time reduction | Timing resolved |
|---|---|---:|---:|---:|---|
| bundle3 | legacy-g32 | 1.014× | 1.002–1.026× | 1.41% | yes |
| bundle3 | generic-h1-mandatory-1 | 1.134× | 1.125–1.142× | 11.81% | yes |
| generic-h1-mandatory-1 | legacy-g32 | 0.895× | 0.888–0.902× | -11.79% | yes |
| minimum-optimized-steady-state | legacy-g32 | 0.958× | 0.952–0.965× | -4.37% | yes |
| minimum-optimized-steady-state | generic-h1-mandatory-1 | 1.071× | 1.066–1.076× | 6.64% | yes |
| minimum-response-count | legacy-g32 | 1.229× | 1.218–1.241× | 18.61% | yes |
| minimum-response-count | generic-h1-mandatory-1 | 1.374× | 1.364–1.384× | 27.20% | yes |
| generic-h1-frontier-1 | legacy-g32 | 0.891× | 0.884–0.899× | -12.18% | yes |
| generic-h1-frontier-1 | generic-h1-mandatory-1 | 0.996× | 0.989–1.004× | -0.35% | no |
| generic-h1-frontier-2 | legacy-g32 | 0.883× | 0.877–0.889× | -13.31% | yes |
| generic-h1-frontier-2 | generic-h1-mandatory-1 | 0.987× | 0.981–0.992× | -1.36% | yes |
| frontier-1 | legacy-g32 | 1.232× | 1.224–1.241× | 18.82% | yes |
| frontier-1 | generic-h1-mandatory-1 | 1.377× | 1.367–1.387× | 27.38% | yes |
| frontier-2 | legacy-g32 | 1.224× | 1.214–1.233× | 18.29% | yes |
| frontier-2 | generic-h1-mandatory-1 | 1.368× | 1.361–1.376× | 26.91% | yes |

Selected measured plan: **minimum-response-count**. statistically unresolved or within 2%; storage tie-break applied.

## Limitations

same recorded leaf schedule and randomized candidate batches; OS drift remains. Only eligible measured candidates are ranked. Coefficient storage estimates are separate from measured process peak RSS. Query sizes are modeled; response sizes count serialized small-q ciphertext payload only.
