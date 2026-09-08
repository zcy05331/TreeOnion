# Tree g32 plaintext-modulus comparison

Full-tree size: 0.249969 MiB; 32B entries; 3 measurements and 1 warmups.

| Scheme | Mean server ms | MiB/s |
|---|---:|---:|
| t4099 / c12 | 103.321 | 2.419 |
| t257 / c8 | 104.193 | 2.399 |

| Stage | t4099 ms | t257 ms | Saved ms |
|---|---:|---:|---:|
| fold | 0.000 | 0.000 | 0.000 |
| pack | 0.086 | 0.137 | -0.051 |
| project | 15.184 | 15.188 | -0.004 |
| pyramid | 14.352 | 14.548 | -0.196 |
| query_expand | 28.359 | 28.786 | -0.427 |
| rotate | 17.544 | 17.845 | -0.301 |
| scan_core | 6.172 | 6.178 | -0.005 |
| scan_finalize | 2.751 | 2.734 | 0.018 |
| selector_completion | 18.071 | 17.982 | 0.089 |
| switch | 0.610 | 0.604 | 0.006 |

| Scheme | Query modeled B | Response actual B | Keys modeled B | Min noise bits |
|---|---:|---:|---:|---:|
| t=4099 | 14880 | 11264 | 1607040 | 3 |
| t=257 | 14880 | 11264 | 1607040 | 7 |

Query and helper-key bytes are modeled; response bytes are serialized.
Both runs use the same query leaf schedule and public layout. Runs are sequential, not interleaved.
Noise and full-response correctness checks are outside all timed stages.
Acceptance requires correct decryption; recorded noise margins are not a pass/fail threshold.
