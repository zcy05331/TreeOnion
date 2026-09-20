# Native campaign launched

Host: zcy05331@clnode273.clemson.cloudlab.us
Remote root: /users/zcy05331/tree-h1-old-layout-20260920T2003Z
Session: tree-h1-matrix-20260920
Core source: 0b20cdbbff271b27f7cfa494f7157e048426b7da
Controller source: 030e0fb1b78d6389cce3a0fbc1e7a459ae313652

Native default 58/58 regression passed. Native EP3/drop32 exact-KS8
multi-plane crypto gate and split-column smoke also passed.

The finite queue now runs 1/4 GiB old-layout h1 tuning, both query-buffer
layouts at the selected g/N0, core baselines, tuned legacy TreePIR backends,
and six external modes. Each heavy operation checks host idleness first.
No other project's processes were signaled. Standard 1 GiB is reused.
The one-time app startup automation was removed once the remote queue took over.

First completed pilot: 1 GiB tier, 32-byte entries, h1/g8/P3/N0=512/b7,
MLWE, interleaved query buffer, 3 warmups then 8 measured queries, seed7.
All queries correctly decrypted, full unused-coefficient checks included.
Mean full server time: 1065.396325 ms; original logical DB throughput:
961.144643 MiB/s. Actual response payload:
11264 bytes/query.
This is a pilot, not the selected optimum or the 50-sample formal result.

Read metadata/status and metadata/primary-progress.json on the remote host
for live state. Local launch-metadata is only a snapshot. External native
builds and H26 encrypted runs remain pending; their partial/failed states
must not be presented as completed measurements.
