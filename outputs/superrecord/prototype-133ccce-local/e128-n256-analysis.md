# Generic super-record local comparison

Run class: `formal_candidate_selection`. 16 measured blocks, 3 warmups. Raw data: [e128-n256.json](e128-n256.json).

| Candidate | h / G / P; b; N0 | Mean ms | Median ms | p95 ms | SD ms | Estimated steady-state MiB |
|---|---:|---:|---:|---:|---:|---:|
| generic-h1-mandatory-0 | 1 / 256 / 1; 2; 8 | 31.771 | 31.724 | 32.319 | 0.237 | 12.70 |
| minimum-optimized-steady-state | 8 / 128 / 1; 1; 1 | 19.247 | 19.215 | 19.502 | 0.158 | 8.05 |
| minimum-response-count | 8 / 128 / 1; 0; 2 | 18.928 | 18.904 | 19.231 | 0.182 | 8.16 |
| generic-h1-frontier-1 | 1 / 128 / 1; 1; 8 | 32.373 | 32.350 | 32.801 | 0.245 | 12.12 |
| generic-h1-frontier-2 | 1 / 128 / 1; 2; 4 | 32.438 | 32.461 | 32.721 | 0.196 | 11.43 |
| frontier-1 | 8 / 256 / 1; 1; 2 | 18.858 | 18.837 | 19.109 | 0.143 | 8.27 |
| frontier-2 | 8 / 256 / 1; 2; 1 | 19.564 | 19.563 | 20.028 | 0.220 | 8.19 |

| Candidate | Control | Speedup | Paired 95% interval | Time reduction | Timing resolved |
|---|---|---:|---:|---:|---|
| minimum-optimized-steady-state | generic-h1-mandatory-0 | 1.651× | 1.643–1.659× | 39.42% | yes |
| minimum-response-count | generic-h1-mandatory-0 | 1.678× | 1.668–1.690× | 40.42% | yes |
| generic-h1-frontier-1 | generic-h1-mandatory-0 | 0.981× | 0.977–0.987× | -1.90% | yes |
| generic-h1-frontier-2 | generic-h1-mandatory-0 | 0.979× | 0.975–0.985× | -2.10% | yes |
| frontier-1 | generic-h1-mandatory-0 | 1.685× | 1.675–1.695× | 40.64% | yes |
| frontier-2 | generic-h1-mandatory-0 | 1.624× | 1.613–1.635× | 38.42% | yes |

Selected measured plan: **minimum-response-count**. statistically unresolved or within 2%; storage tie-break applied; storage within 2%, response count tie-break applied.

## Limitations

same leaf per shared measurement block; randomized candidate order within every block; OS drift remains. Only eligible measured candidates are ranked. Coefficient storage estimates are separate from measured process peak RSS. Query sizes are modeled; response sizes count serialized small-q ciphertext payload only.
