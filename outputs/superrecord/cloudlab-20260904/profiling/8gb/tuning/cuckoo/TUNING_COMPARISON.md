# Cuckoo 8 GiB Layout Pilot Comparison

Four forced-expansion Cuckoo pilot layouts, 16 measured pilot samples each with 3 warmups. These are pilot measurements, not 100-sample formal results.

Winner by mean complete-path server time: h_q=10 (N0=512).

Run context: these h_q pilots ran serially on CloudLab; this report uses recovered raw files. The formal 100-sample tuned Cuckoo result has just started and is not included here.

## Complete-Path Server Metrics

| height | N0 | mean server s | median server s | p95 server s | sample SD s | speedup vs h_q=7 | query modeled | response actual | setup keys modeled | estimated DB size |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| h_q=7 | 32 | 159.486 | 159.474 | 159.604 | 0.079 | 1.000x | 610.31 KiB | 462.00 KiB | 1.42 MiB | 124.99 GiB |
| h_q=8 | 128 | 58.350 | 58.360 | 58.443 | 0.041 | 2.733x | 610.31 KiB | 462.00 KiB | 1.42 MiB | 125.02 GiB |
| h_q=9 | 256 | 44.253 | 44.254 | 44.305 | 0.031 | 3.604x | 610.31 KiB | 462.00 KiB | 1.42 MiB | 125.03 GiB |
| h_q=10 | 512 | 41.217 | 41.217 | 41.239 | 0.018 | 3.869x | 610.31 KiB | 462.00 KiB | 1.42 MiB | 125.05 GiB |

## Eight-Stage Server Means

| height | N0 | expand s | convert s | first dim query ntt s | first dim query pack s | first dim core s | first dim finalize s | other dim s | mod switch s |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| h_q=7 | 32 | 1.354 | 1.653 | 0.034 | 3.315 | 35.112 | 41.697 | 75.620 | 0.026 |
| h_q=8 | 128 | 2.478 | 1.398 | 0.137 | 1.216 | 23.424 | 10.582 | 18.909 | 0.026 |
| h_q=9 | 256 | 3.881 | 1.286 | 0.277 | 1.106 | 23.603 | 4.635 | 9.425 | 0.026 |
| h_q=10 | 512 | 7.684 | 1.169 | 0.572 | 1.875 | 22.625 | 2.453 | 4.706 | 0.026 |

## Figures

- `stage-comparison.png`
- `stage-comparison.pdf`

Selection was verified against `selection.json` using mean complete-path server time. No paired confidence interval is reported because the pilots are not an interleaved paired-block design.
