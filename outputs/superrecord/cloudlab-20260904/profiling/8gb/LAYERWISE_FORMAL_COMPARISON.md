# 8 GiB Layerwise formal comparison

Both configurations completed 100 correct measured paths after 3 warmups on CPU 0 with interleaved NUMA memory.

Average server time fell from 20.702 s to 12.331 s: 40.434% less time, or 1.679x useful throughput.

| Stage | Previous mean ms | Tuned mean ms |
| --- | ---: | ---: |
| expand | 531.176 | 1131.104 |
| convert | 393.624 | 326.213 |
| first_dim_query_ntt | 26.811 | 85.778 |
| first_dim_query_pack | 369.059 | 308.343 |
| first_dim_core | 8517.114 | 7715.641 |
| first_dim_finalize | 3895.062 | 963.797 |
| other_dim | 6900.359 | 1770.342 |
| mod_switch | 12.890 | 12.888 |

Online communication is unchanged: 312480 modeled query bytes and 240576 actual response bytes per complete path. One-time helper keys are modeled at 1488000 bytes.

The measurements ran in separate processes; the speedup is a ratio of observed arithmetic means. The complete 8 GiB matrix remains in progress.
