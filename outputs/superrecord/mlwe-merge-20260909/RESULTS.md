# Wisconsin MLWE response merge results

Source: 0ac2393c09c597b5bb1f41129577117a43f4770a. Native Xeon Silver 4314, CPU 0; 1 GiB logical tree, 32 B entries, EP2/drop36, exact KS8. Each method/layout has 100 measured queries (two reversed-order rounds of 50), six warmups. Every requested path decoded correctly.

| Layout | Projection server ms | MLWE server ms | Throughput gain | Projection MiB/s | MLWE MiB/s |
|---|---:|---:|---:|---:|---:|
| h3 | 828.712 | 817.910 | +1.32% | 1235.65 | 1251.97 |
| h1 | 965.707 | 894.771 | +7.93% | 1060.36 | 1144.43 |

| Stage (ms) | h3 Projection | h3 MLWE | h1 Projection | h1 MLWE |
|---|---:|---:|---:|---:|
| expand | 56.190 | 56.134 | 76.280 | 76.411 |
| complete | 13.534 | 13.560 | 14.295 | 14.332 |
| prepare | 26.241 | 26.348 | 39.828 | 39.496 |
| scan | 567.508 | 559.069 | 634.103 | 615.130 |
| finalize | 73.995 | 82.255 | 50.404 | 51.796 |
| fold | 68.357 | 68.151 | 50.285 | 50.348 |
| rotate | 8.512 | 8.348 | 36.536 | 36.188 |
| project | 13.983 | 0.000 | 63.245 | 0.000 |
| pack | 0.132 | 0.000 | 0.397 | 0.000 |
| merge | 0.000 | 3.785 | 0.000 | 10.735 |
| switch | 0.096 | 0.096 | 0.096 | 0.096 |

Both methods use the same database layout, query schedule, q/t, gadget parameters and registered key family. h3 is G64/P1/N0=352/b9; h1 is G8/P3/N0=512/b7. The current planner was not retuned for MLWE.

Projection/merge KS counts: h3 105 -> 27; h1 492 -> 79. Private rotation remains unchanged. The new merge includes normalization, public shifts, additions and shared BV key switches. Its input fragments use public bit-reversed residue positions at decode.

Communication is unchanged: each reply contains 11,264 ciphertext-payload bytes; seeded query/key costs retain their existing modeled definitions. Framing/packing metadata is not included in the ciphertext payload. Peak process RSS (including setup) is recorded in SUMMARY.json.

Local 256 MiB Rosetta results are preserved in LOCAL_RESULTS.md: the changed response stage became faster, but full-query time did not improve during concurrent local history-cleanup work. Native results above are the dedicated-node comparison.

For h1, the changed Projection/packing -> merge stages directly save 52.907 ms. The measured full-query saving is 70.936 ms, and the scan also measures 18.974 ms faster. The scan kernel itself is unchanged; this additional difference has not been attributed to cache, allocation or frequency effects. Thus 7.93% is the observed whole-query throughput gain for this fixed configuration, not a claim that fewer KS alone account for all of it.
