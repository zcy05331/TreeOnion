# Tree g32 plaintext-modulus comparison

Full-tree size: 0.249969 MiB; 32B entries; 3 measurements and 1 warmups.

| Scheme | Mean server ms | MiB/s |
|---|---:|---:|
| t4099 / c12 | 50.653 | 4.935 |
| t257 / c8 | 51.078 | 4.894 |

| Stage | t4099 ms | t257 ms | Saved ms |
|---|---:|---:|---:|
| fold | 0.000 | 0.000 | -0.000 |
| pack | 0.040 | 0.040 | 0.000 |
| project | 8.199 | 8.643 | -0.444 |
| pyramid | 3.985 | 4.033 | -0.048 |
| query_expand | 15.228 | 15.311 | -0.083 |
| rotate | 10.458 | 10.694 | -0.236 |
| scan_core | 0.718 | 0.875 | -0.157 |
| scan_finalize | 0.476 | 0.468 | 0.008 |
| selector_completion | 11.033 | 10.499 | 0.534 |
| switch | 0.394 | 0.367 | 0.028 |

| Scheme | Query modeled B | Response actual B | Keys modeled B | Min noise bits |
|---|---:|---:|---:|---:|
| t=4099 | 14880 | 11264 | 1607040 | 3 |
| t=257 | 14880 | 11264 | 1607040 | 7 |

Query and helper-key bytes are modeled; response bytes are serialized.
Both runs use the same query leaf schedule and public layout. Runs are sequential, not interleaved.
Noise and full-response correctness checks are outside all timed stages.
