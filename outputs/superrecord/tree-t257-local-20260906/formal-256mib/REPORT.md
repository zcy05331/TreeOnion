# Tree g32 plaintext-modulus comparison

Full-tree size: 255.999969 MiB; 32B entries; 100 measurements and 3 warmups.

| Scheme | Mean server ms | MiB/s |
|---|---:|---:|
| t4099 / c12 | 795.114 | 321.966 |
| t257 / c8 | 698.209 | 366.652 |

| Stage | t4099 ms | t257 ms | Saved ms |
|---|---:|---:|---:|
| fold | 47.660 | 44.813 | 2.847 |
| pack | 0.084 | 0.083 | 0.002 |
| project | 18.180 | 17.753 | 0.427 |
| pyramid | 53.996 | 47.593 | 6.403 |
| query_expand | 92.107 | 89.042 | 3.066 |
| rotate | 23.428 | 21.843 | 1.584 |
| scan_core | 519.036 | 440.523 | 78.513 |
| scan_finalize | 12.871 | 11.335 | 1.536 |
| selector_completion | 24.363 | 23.326 | 1.037 |
| switch | 0.383 | 0.382 | 0.001 |

| Scheme | Query modeled B | Response actual B | Keys modeled B | Min noise bits |
|---|---:|---:|---:|---:|
| t=4099 | 14880 | 11264 | 1607040 | 1 |
| t=257 | 14880 | 11264 | 1607040 | 5 |

Query and helper-key bytes are modeled; response bytes are serialized.
Both runs use the same query leaf schedule and public layout. Runs are sequential, not interleaved.
Noise and full-response correctness checks are outside all timed stages.
