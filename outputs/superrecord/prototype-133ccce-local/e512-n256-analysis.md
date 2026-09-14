# Generic super-record local comparison

Run class: `formal_candidate_selection`. 16 measured blocks, 3 warmups. Raw data: [e512-n256.json](e512-n256.json).

| Candidate | h / G / P; b; N0 | Mean ms | Median ms | p95 ms | SD ms | Estimated steady-state MiB |
|---|---:|---:|---:|---:|---:|---:|
| generic-h1-mandatory-0 | 1 / 256 / 1; 2; 8 | 31.783 | 31.733 | 32.083 | 0.155 | 12.70 |
| minimum-optimized-steady-state | 8 / 128 / 3; 1; 1 | 30.706 | 30.637 | 32.075 | 0.409 | 8.52 |
| minimum-response-count | 3 / 128 / 1; 1; 3 | 27.693 | 27.661 | 28.110 | 0.177 | 10.26 |
| generic-h1-frontier-1 | 1 / 128 / 1; 1; 8 | 32.522 | 32.485 | 33.013 | 0.181 | 12.12 |
| generic-h1-frontier-2 | 1 / 128 / 1; 2; 4 | 32.453 | 32.451 | 32.669 | 0.157 | 11.43 |
| frontier-1 | 6 / 256 / 1; 1; 3 | 23.314 | 23.283 | 24.275 | 0.296 | 9.54 |
| frontier-2 | 4 / 256 / 1; 1; 4 | 23.296 | 23.274 | 24.072 | 0.273 | 9.67 |

| Candidate | Control | Speedup | Paired 95% interval | Time reduction | Timing resolved |
|---|---|---:|---:|---:|---|
| minimum-optimized-steady-state | generic-h1-mandatory-0 | 1.035× | 1.028–1.041× | 3.39% | yes |
| minimum-response-count | generic-h1-mandatory-0 | 1.148× | 1.143–1.153× | 12.87% | yes |
| generic-h1-frontier-1 | generic-h1-mandatory-0 | 0.977× | 0.975–0.980× | -2.33% | yes |
| generic-h1-frontier-2 | generic-h1-mandatory-0 | 0.979× | 0.976–0.982× | -2.11% | yes |
| frontier-1 | generic-h1-mandatory-0 | 1.363× | 1.355–1.370× | 26.65% | yes |
| frontier-2 | generic-h1-mandatory-0 | 1.364× | 1.358–1.370× | 26.70% | yes |

Selected measured plan: **frontier-2**. statistically unresolved or within 2%; storage tie-break applied; storage within 2%, response count tie-break applied.

## Limitations

same leaf per shared measurement block; randomized candidate order within every block; OS drift remains. Only eligible measured candidates are ranked. Coefficient storage estimates are separate from measured process peak RSS. Query sizes are modeled; response sizes count serialized small-q ciphertext payload only.
