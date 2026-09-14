# NUMA placement at fixed CPU 0

The original measurement executable is used for all four runs, in A-B-B-A order.
Each process performs three warmups and sixteen measured queries with the same leaf schedule.

| Metric | Interleaved across nodes | Local node 0 |
| --- | ---: | ---: |
| Server mean, ms | 3208.297 | 3029.009 |
| convert, ms | 262.407 | 261.787 |
| expand, ms | 394.984 | 391.762 |
| first_dim_core, ms | 1079.518 | 931.748 |
| first_dim_finalize, ms | 470.516 | 458.675 |
| first_dim_query_ntt, ms | 21.636 | 21.581 |
| first_dim_query_pack, ms | 96.530 | 84.963 |
| mod_switch, ms | 11.041 | 11.155 |
| other_dim, ms | 864.688 | 861.490 |

Local-memory latency reduction: 5.59%.
This measures the effect of the memory policy on this machine, not the complete cross-CPU difference.
Per-run numastat snapshots and raw profiling JSON are retained alongside this summary.
