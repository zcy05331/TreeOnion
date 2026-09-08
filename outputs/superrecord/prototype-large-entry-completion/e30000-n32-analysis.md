# Generic super-record local comparison

Run class: `formal_candidate_selection`. 16 measured blocks, 3 warmups. Raw data: [e30000-n32.json](e30000-n32.json).

| Candidate | h / G / P; b; N0 | Mean ms | Median ms | p95 ms | SD ms | Estimated steady-state MiB |
|---|---:|---:|---:|---:|---:|---:|
| generic-h1-mandatory-0 | 1 / 2048 / 2; 1; 16 | 21.882 | 21.856 | 22.475 | 0.272 | 13.38 |
| minimum-optimized-steady-state | 2 / 1024 / 5; 2; 2 | 29.541 | 29.383 | 30.418 | 0.383 | 9.74 |
| generic-h1-mandatory-2 | 1 / 512 / 5; 0; 8 | 34.389 | 34.189 | 35.533 | 0.497 | 11.09 |
| generic-h1-frontier-1 | 1 / 2048 / 2; 2; 8 | 21.834 | 21.738 | 22.648 | 0.328 | 11.95 |
| generic-h1-frontier-2 | 1 / 2048 / 2; 0; 32 | 24.298 | 24.228 | 24.897 | 0.309 | 16.51 |
| frontier-1 | 1 / 2048 / 2; 3; 4 | 24.693 | 24.543 | 25.515 | 0.431 | 11.45 |
| frontier-2 | 1 / 1024 / 3; 1; 8 | 24.970 | 24.841 | 25.807 | 0.346 | 11.32 |

| Candidate | Control | Speedup | Paired 95% interval | Time reduction | Timing resolved |
|---|---|---:|---:|---:|---|
| generic-h1-mandatory-0 | generic-h1-frontier-1 | 0.998× | 0.992–1.004× | -0.22% | no |
| minimum-optimized-steady-state | generic-h1-frontier-1 | 0.739× | 0.735–0.743× | -35.30% | yes |
| generic-h1-mandatory-2 | generic-h1-frontier-1 | 0.635× | 0.632–0.638× | -57.50% | yes |
| generic-h1-frontier-2 | generic-h1-frontier-1 | 0.899× | 0.893–0.904× | -11.29% | yes |
| frontier-1 | generic-h1-frontier-1 | 0.884× | 0.880–0.888× | -13.09% | yes |
| frontier-2 | generic-h1-frontier-1 | 0.874× | 0.868–0.880× | -14.37% | yes |

Selected measured plan: **generic-h1-frontier-1**. statistically unresolved or within 2%; storage tie-break applied.

## Limitations

same leaf per shared measurement block; randomized candidate order within every block; OS drift remains. Only eligible measured candidates are ranked. Coefficient storage estimates are separate from measured process peak RSS. Query sizes are modeled; response sizes count serialized small-q ciphertext payload only.
