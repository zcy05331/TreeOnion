# Legacy g32 across database scales

Each run completed 100 correct measured ancestor paths after three warmups. Both include the root and use 32-byte nodes.

| Tier | Mean server ms | Median ms | Process peak GiB | Useful path bytes |
| --- | ---: | ---: | ---: | ---: |
| 1gb | 2647.043 | 2649.660 | 16.19 | 800 |
| 8gb | 18992.701 | 18990.620 | 128.66 | 896 |

## Stage means

| Stage | 1 GiB ms | 8 GiB ms |
| --- | ---: | ---: |
| fold | 296.844 | 2418.317 |
| pack | 0.180 | 0.332 |
| project | 34.825 | 40.095 |
| pyramid | 131.425 | 131.500 |
| query_expand | 173.137 | 180.419 |
| rotate | 40.769 | 47.747 |
| scan_core | 1531.762 | 11795.876 |
| scan_finalize | 369.643 | 3902.616 |
| selector_completion | 45.644 | 55.036 |
| switch | 0.611 | 0.612 |

## Communication

Each query at both scales uses 14,880 modeled upload bytes and 11,264 actual response bytes. One-time evaluation keys are modeled at 1,607,040 bytes. The process peak includes database setup and evaluation; it is not the database byte count.

The 8 GiB scan core and finalization account for 82.66% of mean server time. These are separate-process measurements on the same CloudLab host, CPU 0 and interleaved NUMA policy. The remaining 8 GiB configurations are still running.
