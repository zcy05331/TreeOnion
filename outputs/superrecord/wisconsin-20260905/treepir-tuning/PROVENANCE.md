# Retrieved TreePIR evidence

1gb/formal.* was retrieved from
/users/zcy05331/core-pir-20260905/outputs/treepir-backend-tuning/1gb/formal.*
on sm110p-10s10611.wisc.cloudlab.us. It is complete, exit0,16 correct measured
queries and3 warmups, selected backendvbpir_fd256. A local copy under
profiling/1gb/treepir.* supplies the aggregate table while the remote wrapper
waits for both tiers before promotion.

4gb/pilot-sealpir_d2*.json is a completed16-query pilot only. Its independent
formal run had started at the time of retrieval and is not represented as a
formal throughput value yet.

Kernel journal confirmed OOM kills of VBPIR PIDs7748 and7756 at05:10:23 and
05:25:02 UTC on2026-09-06. These correspond to4GiB fd64/fd128. Pilot logs
reported the other feasibility/noise failures summarized in REPORT.md.
