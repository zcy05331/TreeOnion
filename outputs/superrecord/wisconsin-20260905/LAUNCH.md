# Wisconsin core experiment launch

Started 2026-09-05 23:30:04 UTC on
`zcy05331@sm110p-10s10611.wisc.cloudlab.us`.

- Run root: `/users/zcy05331/core-pir-20260905`
- Session: `core-1gb-4gb`
- Matrix controller PID at launch: 5286
- First live benchmark PID: 5306 (`layer_layout_sweep`, 1 GiB, 16+3 tuning)
- Source: 10a172d (Onion C++ identical to 278e4dc)
- Environment and matrix: `docs/experiments/wisconsin-20260905/ENVIRONMENT.md`
- Native build and smoke complete, regressions 39/39 applicable + CLI.
- Python regressions: 34 passed.
- Independent native HEXL witness passed 5/5 correctness checks.
- Independent H26 plan-only witness: 100+3, h1+h3, two mandatory controls,
  estimated resident peak 108372456928 bytes (100.93 GiB) under 110 GiB.

The matrix runs 1 GiB and 4 GiB at 32B per entry. Core formal counts:
100 each for Generic h3, Generic h1, Layerwise tuned and Tree g32;
16 each for Cuckoo tuned, TreePIR and Flat; three warmups per configuration.
Generic reports selected measured candidates in a shared sweep.

TreePIR is explicitly pending backend/parameter tuning; the core runner is
configured TREEPIR_SKIP=1 so it can run the other cases while adapters are
prepared. A pending TreePIR case cannot mark the matrix complete.

This is launch evidence, not a result or completion report. Continuous
progress monitoring remains paused.

## TreePIR continuation staged

Revision 1d0c732, 95 KiB scripts/adapter package. Wisconsin session
`treepir-after-core`, controller 5767, status `waiting_for_core`, predecessor
5286. It measures the finite VBPIR/SealPIR grid and performs a fresh formal
16-query run per scale before completing the pending TreePIR rows.

The Clemson interlude was separately staged in session
`treepir-tuning-interlude`, waiting on Generic worker 29463 while the original
parent 18736 remains stopped. It preserves the original 1 GiB result, reruns
1 GiB with 16 samples and completes the already-requested 8 GiB TreePIR with
100 samples before resuming that parent.
