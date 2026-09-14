# Wisconsin sm110p-10s10616 experiment environment

Host: zcy05331@sm110p-10s10616.wisc.cloudlab.us
Run root: /users/zcy05331/wisconsin-alpha-entry-20260908
Observed CPU: Intel Xeon Silver 4314, 32 logical CPUs, one socket and NUMA node,
AVX-512 IFMA available. Approximately 125 GiB RAM and 57 GiB free root disk.
OS: Ubuntu 22.04.2. CMake 3.22.1 and numactl installed for this run.

Use CPU 0, interleave all NUMA nodes, one thread for all measured work.
Intel HEXL is pinned to 75a60b8cc908fb4c166315dd8b55b2e1ee7faea7 (1.2.6),
matching prior Wisconsin runs. No SEAL/TreePIR dependency build is needed.
Build Benchmark binaries with CONFIG_N2048_K1_COMP and native CPU flags.

Build three binaries: original 5a05443 plus only the benchmark shape pin;
current optimized Generic; and the t257 configuration for the legacy g32
comparison. Capture compiler/HEXL/source versions and build/test logs.

Run order, serially:
1. N0=352 alpha preparation microbenchmark, 100 alternating pairs.
2. Fixed h3 alpha before/after at 1 GiB, 32B entries, G64/P1/N0=352/b9.
   Two execution orders with 50 measured queries and 3 warmups each: totals
   are 100 measured queries and 6 warmups per implementation.
3. Optimized Generic h1/h3 entry-size sweeps: 16,48,64,128B. Each measured
   candidate has 100 queries and 3 warmups, minimum mean server time first.
   16/64/128B use about 1 GiB; 48B uses 2^24 leaves and about 1.5 GiB.
4. Legacy g32 t4099/c12 versus t257/c8 at 1 GiB, 100 measured queries and
   3 warmups each, preserving the unfinished Wisconsin comparison scope.

Planner memory limit: 80 GiB. Acceptance uses actual decryption correctness;
noise margins are recorded diagnostically. Preserve per-stage timings,
communication labeling, process peak memory and individual failure records.
Independent stages continue after a recorded stage failure.

A finite local collector reads metadata/progress.json and retrieves each
finished stage, including failed stages with logs. It does not resume any
notification automation. It exits after final synchronization or records an
unreachable node after three consecutive connection failures. Collection
requires this workstation and its collector process to remain running.

Remote launcher: scripts/run_wisconsin_alpha_entries.sh RUN_ROOT
Data runner: scripts/run_wisconsin_alpha_entries.py --run-root RUN_ROOT
Local collector: scripts/collect_cloudlab_results.py --host HOST
  --remote-root RUN_ROOT --local-root LOCAL_ARCHIVE
