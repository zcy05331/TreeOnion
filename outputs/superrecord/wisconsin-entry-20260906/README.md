# Wisconsin Generic entry-size experiment

Requested entries: 16, 48, 64, 128 bytes. Generic h1 and h3 candidates
are measured in interleaved blocks: 100 measurements and 3 warmups each.
Select the fastest measured h1 and h3 by mean server compute time.

| Entry bytes | Leaves | Nominal full-tree capacity | Actual bytes |
|---|---:|---:|---:|
| 16 | 33554432 | 1 GiB | 1073741808 |
| 48 | 16777216 | 1.5 GiB | 1610612688 |
| 64 | 8388608 | 1 GiB | 1073741760 |
| 128 | 4194304 | 1 GiB | 1073741696 |

The user explicitly chose the same leaf count for 48B as the earlier 32B
reference. Throughput uses actual full-tree bytes, not a fixed 1 GiB
numerator for the 48B row. Generic returns all ancestor levels including root.

Host: zcy05331@sm110p-10s10611.wisc.cloudlab.us, Xeon Silver 4314.
Run directory: /users/zcy05331/generic-entry-wisconsin-20260906.
Reuse the original core native binary from /users/zcy05331/core-pir-20260905,
compiled from 10a172d, to preserve comparison with the prior Wisconsin rows.
CPU 0, interleave all memory nodes, one thread, 80 GiB planner memory budget.
No new dependency build or change to the crypto implementation.

The scripts validate four small encrypted cases before the full runs.
Each full run retains raw samples, per-stage timings, modeled seeded query
bytes and evaluation-key bytes, actual serialized response bytes, process
exit status, peak memory, and a local report. A failed formal entry is recorded
and does not suppress independent sizes. Full results are only complete
when their raw correctness, sample counts and process checks pass.

Launch with the scripts snapshot placed under RUN_ROOT/source:

```bash
bash RUN_ROOT/source/scripts/run_wisconsin_entry_sizes.sh RUN_ROOT CORE_RUN_ROOT
```

This file describes the experiment plan; consult metadata/status and
outputs/formal/manifest.json in the synchronized run archive for progress.
