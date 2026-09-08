# Tree g32 plaintext-modulus comparison

Full-tree size: 1023.999969 MiB; 32B entries; 100 measurements and 3 warmups.

| Scheme | Mean server ms | MiB/s |
|---|---:|---:|
| t4099 / c12 | 2635.377 | 388.559 |
| t257 / c8 | 2635.821 | 388.494 |

| Stage | t4099 ms | t257 ms | Saved ms |
|---|---:|---:|---:|
| fold | 298.642 | 299.630 | -0.988 |
| pack | 0.182 | 0.289 | -0.107 |
| project | 33.471 | 33.611 | -0.140 |
| pyramid | 132.241 | 131.827 | 0.414 |
| query_expand | 166.672 | 169.163 | -2.490 |
| rotate | 40.916 | 41.271 | -0.355 |
| scan_core | 1507.994 | 1507.437 | 0.557 |
| scan_finalize | 385.715 | 382.979 | 2.736 |
| selector_completion | 45.874 | 46.091 | -0.216 |
| switch | 0.613 | 0.612 | 0.001 |

| Scheme | Query modeled B | Response actual B | Keys modeled B | Min noise bits |
|---|---:|---:|---:|---:|
| t=4099 | 14880 | 11264 | 1607040 | 1 |
| t=257 | 14880 | 11264 | 1607040 | 5 |

Query and helper-key bytes are modeled; response bytes are serialized.
Both runs use the same query leaf schedule and public layout. Runs are sequential, not interleaved.
Noise and full-response correctness checks are outside all timed stages.
Acceptance requires correct decryption; recorded noise margins are not a pass/fail threshold.
