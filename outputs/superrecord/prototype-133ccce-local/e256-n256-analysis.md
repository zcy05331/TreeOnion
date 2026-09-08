# Generic super-record local comparison

Run class: `formal_candidate_selection`. 16 measured blocks, 3 warmups. Raw data: [e256-n256.json](e256-n256.json).

| Candidate | h / G / P; b; N0 | Mean ms | Median ms | p95 ms | SD ms | Estimated steady-state MiB |
|---|---:|---:|---:|---:|---:|---:|
| bundle3 | 3 / 64 / 1; 1; 2 | 28.869 | 28.828 | 29.389 | 0.215 | 9.87 |
| generic-h1-mandatory-1 | 1 / 256 / 1; 2; 8 | 32.507 | 32.479 | 33.348 | 0.336 | 12.70 |
| minimum-optimized-steady-state | 8 / 256 / 1; 2; 1 | 19.982 | 20.019 | 20.170 | 0.128 | 8.19 |
| minimum-response-count | 6 / 128 / 1; 0; 3 | 24.042 | 24.053 | 24.189 | 0.103 | 9.40 |
| generic-h1-frontier-1 | 1 / 128 / 1; 1; 8 | 33.765 | 32.852 | 46.921 | 3.516 | 12.12 |
| generic-h1-frontier-2 | 1 / 128 / 1; 2; 4 | 33.404 | 33.023 | 37.452 | 1.109 | 11.43 |
| frontier-1 | 8 / 256 / 1; 1; 2 | 19.270 | 19.292 | 19.522 | 0.179 | 8.27 |
| frontier-2 | 8 / 256 / 1; 0; 4 | 20.027 | 19.982 | 20.256 | 0.126 | 8.90 |
| legacy-g32 | — / — / —; —; — | 28.927 | 28.984 | 29.175 | 0.174 | unavailable |

| Candidate | Control | Speedup | Paired 95% interval | Time reduction | Timing resolved |
|---|---|---:|---:|---:|---|
| bundle3 | legacy-g32 | 1.002× | 0.997–1.007× | 0.20% | no |
| bundle3 | generic-h1-mandatory-1 | 1.126× | 1.119–1.134× | 11.19% | yes |
| generic-h1-mandatory-1 | legacy-g32 | 0.890× | 0.885–0.895× | -12.38% | yes |
| minimum-optimized-steady-state | legacy-g32 | 1.448× | 1.441–1.454× | 30.92% | yes |
| minimum-optimized-steady-state | generic-h1-mandatory-1 | 1.627× | 1.617–1.637× | 38.53% | yes |
| minimum-response-count | legacy-g32 | 1.203× | 1.199–1.207× | 16.89% | yes |
| minimum-response-count | generic-h1-mandatory-1 | 1.352× | 1.345–1.359× | 26.04% | yes |
| generic-h1-frontier-1 | legacy-g32 | 0.857× | 0.813–0.882× | -16.72% | yes |
| generic-h1-frontier-1 | generic-h1-mandatory-1 | 0.963× | 0.917–0.990× | -3.87% | yes |
| generic-h1-frontier-2 | legacy-g32 | 0.866× | 0.850–0.876× | -15.48% | yes |
| generic-h1-frontier-2 | generic-h1-mandatory-1 | 0.973× | 0.959–0.983× | -2.76% | yes |
| frontier-1 | legacy-g32 | 1.501× | 1.496–1.507× | 33.38% | yes |
| frontier-1 | generic-h1-mandatory-1 | 1.687× | 1.675–1.700× | 40.72% | yes |
| frontier-2 | legacy-g32 | 1.444× | 1.438–1.450× | 30.77% | yes |
| frontier-2 | generic-h1-mandatory-1 | 1.623× | 1.617–1.629× | 38.39% | yes |

Selected measured plan: **frontier-1**. smallest arithmetic mean; timing comparison resolved.

## Limitations

same leaf per shared measurement block; randomized candidate order within every block; OS drift remains. Only eligible measured candidates are ranked. Coefficient storage estimates are separate from measured process peak RSS. Query sizes are modeled; response sizes count serialized small-q ciphertext payload only.
