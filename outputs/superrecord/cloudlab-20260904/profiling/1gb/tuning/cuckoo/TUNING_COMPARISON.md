# Cuckoo 1 GiB Layout Pilot Comparison

Four forced-expansion Cuckoo pilot layouts, 16 measured pilot samples each with 3 warmups. These are pilot measurements, not 100-sample formal results.

Winner by mean complete-path server time: h9 (N0=256).

Run context: the 20m8.7s pause was the generic preflight, not these N0 pilots; the N0 pilots ran serially against each other. The generic run has resumed, and the formal 100-sample tuned Cuckoo result has not completed yet.

## Complete-Path Server Metrics

| height | N0 | mean server s | median server s | p95 server s | sample SD s | speedup vs N0=64 | query modeled | response actual | setup keys modeled | physical DB bytes |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| h_q=7 | 64 | 13.154 | 13.154 | 13.169 | 0.008 | 1.000x | 537.66 KiB | 407.00 KiB | 1.42 MiB | 15.59 GiB |
| h_q=8 | 128 | 9.991 | 9.990 | 10.004 | 0.008 | 1.317x | 537.66 KiB | 407.00 KiB | 1.42 MiB | 15.61 GiB |
| h_q=9 | 256 | 9.792 | 9.791 | 9.798 | 0.004 | 1.343x | 537.66 KiB | 407.00 KiB | 1.42 MiB | 15.61 GiB |
| h_q=10 | 512 | 12.181 | 12.181 | 12.186 | 0.002 | 1.080x | 537.66 KiB | 407.00 KiB | 1.42 MiB | 15.61 GiB |

## Eight-Stage Server Means

| height | N0 | expand s | convert s | first dim query ntt s | first dim query pack s | first dim core s | first dim finalize s | other dim s | mod switch s |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| h_q=7 | 64 | 1.277 | 1.011 | 0.060 | 0.359 | 3.431 | 2.304 | 4.684 | 0.023 |
| h_q=8 | 128 | 1.890 | 0.897 | 0.121 | 0.401 | 2.967 | 1.345 | 2.344 | 0.023 |
| h_q=9 | 256 | 3.235 | 0.790 | 0.243 | 0.661 | 2.993 | 0.674 | 1.168 | 0.023 |
| h_q=10 | 512 | 5.970 | 0.688 | 0.497 | 1.256 | 2.869 | 0.287 | 0.584 | 0.023 |

## Figures

- `stage-comparison.png`
- `stage-comparison.pdf`

Selection was verified against `selection.json` using mean complete-path server time. No paired confidence interval is reported because the pilots are not an interleaved paired-block design.
