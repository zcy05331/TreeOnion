# Scalar g1 diagnostic

These runs use 12-bit nodes and return an ancestor path including the root. Their speed is not a 32-byte-node baseline result. Each run completed 100 correct measured queries after 3 warmups.

| Reference tier | Actual logical payload MiB | Mean server ms | Process peak GiB |
| --- | ---: | ---: | ---: |
| 1gb | 48.000 | 407.035 | 0.55 |
| 8gb | 384.000 | 1404.789 | 4.10 |

At both reference tiers, each query uses 14,880 modeled upload bytes and 11,264 actual response bytes. Evaluation keys are modeled at 1,607,040 bytes once per setup.

The reference tier names describe the tree sizes used by 32-byte-node experiments; the actual scalar payloads here are approximately 48 MiB and 384 MiB.
