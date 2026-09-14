# Generic super-record local comparison

Run class: `formal_candidate_selection`. 16 measured blocks, 3 warmups. Raw data: [e256-n4096.json](e256-n4096.json).

| Candidate | h / G / P | Mean ms | Median ms | p95 ms | SD ms | Estimated steady-state MiB |
|---|---:|---:|---:|---:|---:|---:|
| bundle3 | 3 / 64 / 1 | 48.144 | 48.102 | 48.423 | 0.213 | 16.30 |
| generic-h1-mandatory-1 | 1 / 64 / 1 | 55.238 | 55.052 | 59.197 | 1.101 | 20.52 |
| minimum-optimized-steady-state | 6 / 128 / 1 | 45.389 | 45.494 | 46.174 | 0.423 | 14.15 |
| minimum-response-count | 6 / 128 / 1 | 41.708 | 41.680 | 42.188 | 0.243 | 15.55 |
| generic-h1-frontier-1 | 1 / 32 / 1 | 55.533 | 55.494 | 56.493 | 0.531 | 16.91 |
| generic-h1-frontier-2 | 1 / 32 / 1 | 55.337 | 55.325 | 55.842 | 0.296 | 18.41 |
| frontier-1 | 6 / 128 / 1 | 42.892 | 43.039 | 43.342 | 0.323 | 14.77 |
| frontier-2 | 5 / 128 / 1 | 43.106 | 42.720 | 46.225 | 1.011 | 16.27 |
| legacy-g32 | — / — / — | 48.723 | 47.808 | 55.645 | 2.409 | unavailable |

| Candidate | Control | Speedup | Paired 95% interval | Time reduction | Timing resolved |
|---|---|---:|---:|---:|---|
| bundle3 | legacy-g32 | 1.012× | 0.992–1.039× | 1.19% | no |
| bundle3 | generic-h1-mandatory-1 | 1.147× | 1.140–1.159× | 12.84% | yes |
| generic-h1-mandatory-1 | legacy-g32 | 0.882× | 0.861–0.907× | -13.37% | yes |
| minimum-optimized-steady-state | legacy-g32 | 1.073× | 1.053–1.100× | 6.84% | yes |
| minimum-optimized-steady-state | generic-h1-mandatory-1 | 1.217× | 1.206–1.233× | 17.83% | yes |
| minimum-response-count | legacy-g32 | 1.168× | 1.143–1.201× | 14.40% | yes |
| minimum-response-count | generic-h1-mandatory-1 | 1.324× | 1.314–1.340× | 24.49% | yes |
| generic-h1-frontier-1 | legacy-g32 | 0.877× | 0.859–0.902× | -13.98% | yes |
| generic-h1-frontier-1 | generic-h1-mandatory-1 | 0.995× | 0.986–1.007× | -0.54% | no |
| generic-h1-frontier-2 | legacy-g32 | 0.880× | 0.863–0.903× | -13.58% | yes |
| generic-h1-frontier-2 | generic-h1-mandatory-1 | 0.998× | 0.990–1.010× | -0.18% | no |
| frontier-1 | legacy-g32 | 1.136× | 1.112–1.167× | 11.97% | yes |
| frontier-1 | generic-h1-mandatory-1 | 1.288× | 1.279–1.300× | 22.35% | yes |
| frontier-2 | legacy-g32 | 1.130× | 1.104–1.161× | 11.53% | yes |
| frontier-2 | generic-h1-mandatory-1 | 1.281× | 1.264–1.299× | 21.96% | yes |

Selected measured plan: **minimum-response-count**. smallest arithmetic mean; timing comparison resolved.

## Limitations

same recorded leaf schedule and randomized candidate batches; OS drift remains. Only eligible measured candidates are ranked. Coefficient storage estimates are separate from measured process peak RSS. Query sizes are modeled; response sizes count serialized small-q ciphertext payload only.
