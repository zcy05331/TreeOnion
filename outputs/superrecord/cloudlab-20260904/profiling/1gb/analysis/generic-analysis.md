# Generic super-record: CloudLab 1 GiB comparison

Run class: `formal_candidate_selection`. 100 measured blocks, 3 warmups. Raw data: [../generic.json](../generic.json).

| Candidate | h / G / P; b; N0 | Mean ms | Median ms | p95 ms | SD ms | Estimated steady-state MiB |
|---|---:|---:|---:|---:|---:|---:|
| bundle3 | 3 / 64 / 1; 9; 352 | 2010.239 | 2010.257 | 2015.120 | 2.678 | 5741.07 |
| generic-h1-mandatory-1 | 1 / 8 / 3; 7; 512 | 2280.484 | 2280.510 | 2285.160 | 2.864 | 6282.54 |
| bundle3-variant-2 | 3 / 64 / 1; 10; 171 | 3564.439 | 3564.235 | 3571.434 | 3.440 | 5560.57 |
| generic-h1-frontier-1 | 1 / 32 / 1; 9; 512 | 2600.132 | 2599.663 | 2608.257 | 4.119 | 8334.29 |
| generic-h1-frontier-2 | 1 / 16 / 2; 8; 512 | 2699.904 | 2700.287 | 2707.540 | 5.037 | 8334.39 |
| frontier-1 | 3 / 32 / 2; 8; 352 | 2073.534 | 2073.094 | 2079.393 | 2.681 | 5741.21 |
| frontier-2 | 3 / 64 / 1; 9; 342 | 2190.869 | 2190.433 | 2195.865 | 3.201 | 5579.06 |

| Candidate | Control | Speedup | Paired 95% interval | Time reduction | Timing resolved |
|---|---|---:|---:|---:|---|
| bundle3 | generic-h1-mandatory-1 | 1.134× | 1.134–1.135× | 11.85% | yes |
| bundle3-variant-2 | generic-h1-mandatory-1 | 0.640× | 0.640–0.640× | -56.30% | yes |
| generic-h1-frontier-1 | generic-h1-mandatory-1 | 0.877× | 0.877–0.877× | -14.02% | yes |
| generic-h1-frontier-2 | generic-h1-mandatory-1 | 0.845× | 0.844–0.845× | -18.39% | yes |
| frontier-1 | generic-h1-mandatory-1 | 1.100× | 1.099–1.100× | 9.07% | yes |
| frontier-2 | generic-h1-mandatory-1 | 1.041× | 1.041–1.041× | 3.93% | yes |

Selected measured plan: **bundle3**. smallest arithmetic mean; timing comparison resolved.

## Limitations

same leaf per shared measurement block; randomized candidate order within every block; OS drift remains. Only eligible measured candidates are ranked. Coefficient storage estimates are separate from measured process peak RSS. Query sizes are modeled; response sizes count serialized small-q ciphertext payload only.
