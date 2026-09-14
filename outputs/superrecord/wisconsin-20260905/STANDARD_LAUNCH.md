# Standard OnionPIRv2 measurements requested on Wisconsin

Requested 2026-09-06 UTC. Script revision: 2f2e353.
Server: zcy05331@sm110p-10s10611.wisc.cloudlab.us.
Run root: /users/zcy05331/core-pir-20260905.

Two tiers, 32 B nodes: 1 GiB (H24, 16777216 leaves) and 4 GiB
(H26, 67108864 leaves). Each runs 100 measured queries after 3 warmups,
seed 7, on CPU 0 with one thread. The existing native binary is reused
(commit 10a172d for core source/scripts; C++ same as 278e4dc).
Case: merkle_benchmarks / standard_onionpir, original Standard layout.
Return unit is one contiguous 3072 B plaintext block, not a complete path.

Insertion was verified at 2026-09-06 00:35:33 UTC:
- Core parent 5286 is stopped, preserving its active child.
- Current 4 GiB g32 worker 6225 keeps running.
- New tmux session: standard-reference.
- Standard controller 6276 waits for worker 6225 to finish.
- TreePIR queue still waits for core parent 5286; it cannot overlap Standard.
- Standard completion or failure resumes the original core parent.

Result files: outputs/profiling/{1gb,4gb}/standard.json,
standard-process.json and standard.log under the remote run root.
Status: metadata/standard.status and metadata/standard-queue.log.
Full stage timing, actual response bytes, modeled query/key bytes and
correctness are emitted by the existing benchmark implementation.

This is verified queue-launch evidence; both requested Standard measurements
were pending at the time this note was written. Periodic monitoring remains off.
