# Legacy compressed response comparison

Each configuration completed 100 correct full ancestor paths, including the root, after three warmups. Entries contain 32 bytes.

| Tier | Variant | Mean server ms | Mean client decode ms | Actual response bytes | Extra compression-key bytes (modeled) |
| --- | --- | ---: | ---: | ---: | ---: |
| 1gb | tree_g32 | 2647.043 | 0.347 | 11264 | 0 |
| 1gb | tree_compressed | 2635.122 | 1.553 | 16632 | 65560 |
| 8gb | tree_g32 | 18992.701 | 0.347 | 11264 | 0 |
| 8gb | tree_compressed | 18996.021 | 1.552 | 16656 | 65560 |

At both scales, the query model is 14,880 bytes and the ordinary helper/evaluation-key model is 1,607,040 bytes. The compressed variant adds a 65,560-byte one-time compression key, for 1,672,600 modeled setup bytes.

The 8 GiB compressed response is 47.87% larger than the ordinary response (16,656 versus 11,264 bytes). Mean server times differ by only 3.319 ms, about 0.017% of the ordinary mean; these separate-process measurements show no useful server-time or serialized-response-size gain for this prototype.

## 8 GiB stage means

| Stage | Ordinary ms | Compressed ms |
| --- | ---: | ---: |
| fold | 2418.317 | 2415.245 |
| pack | 0.332 | 0.332 |
| project | 40.095 | 40.121 |
| pyramid | 131.500 | 131.588 |
| query_expand | 180.419 | 181.041 |
| rotate | 47.747 | 47.362 |
| scan_core | 11795.876 | 11792.831 |
| scan_finalize | 3902.616 | 3901.820 |
| selector_completion | 55.036 | 55.356 |
| switch | 0.612 | 10.607 |

The `switch` stage contains ordinary same-ring modulus switching for g32;
the compressed variant includes ring/key switching and final rescaling.

The compressed process exited normally, took 2972.608 s including setup, and peaked at 128.66 GiB resident memory. Its database setup timer was 1006.427 s. Raw files contain each measured stage and the separate response serialization/decode timings.
