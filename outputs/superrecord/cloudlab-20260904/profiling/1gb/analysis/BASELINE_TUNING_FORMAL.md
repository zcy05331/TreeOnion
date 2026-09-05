# CloudLab 1 GiB: formal baseline tuning results

All rows below have 100 correct measured paths after 3 warmups. Selection used 16 pilot samples; pilot data remains in the separate tuning directory.

| Baseline | Previous mean s | Tuned mean s | Time reduction | Speedup |
| --- | ---: | ---: | ---: | ---: |
| layerwise | 3.190 | 2.623 | 17.776% | 1.216x |
| cuckoo | 13.182 | 9.630 | 26.948% | 1.369x |

## Layerwise layout changes

| Level | Old N0 | Tuned N0 | Old remaining rows | Tuned remaining rows | Old padded plaintexts | Tuned padded plaintexts |
| ---: | ---: | ---: | ---: | ---: | ---: | ---: |
| 13 | 8 | 32 | 11 | 3 | 88 | 96 |
| 17 | 16 | 64 | 86 | 22 | 1376 | 1408 |
| 18 | 16 | 64 | 171 | 43 | 2736 | 2752 |
| 19 | 64 | 128 | 86 | 43 | 5504 | 5504 |
| 20 | 64 | 128 | 171 | 86 | 10944 | 11008 |
| 21 | 64 | 128 | 342 | 171 | 21888 | 21888 |
| 22 | 64 | 256 | 683 | 171 | 43712 | 43776 |
| 23 | 128 | 256 | 683 | 342 | 87424 | 87552 |
| 24 | 256 | 512 | 683 | 342 | 174848 | 175104 |

Online communication remains unchanged by this tuning: Layerwise uses 267840 modeled query bytes and 206784 actual response bytes; Cuckoo uses 550560 modeled query bytes and 416768 actual response bytes per path. One-time key estimates are retained separately in the raw files.

## Timing scope

The legacy and tuned measurements ran in separate processes on the same CPU/NUMA configuration. Reported speedups are ratios of observed arithmetic means. The tuned Cuckoo uses expansion height 9 and N0=256 for all 37 buckets. Layerwise applies its measured per-level mean-time selection.
