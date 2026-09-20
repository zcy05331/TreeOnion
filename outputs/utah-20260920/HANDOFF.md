# Utah final h1 campaign

Host: zcy05331@er031.utah.cloudlab.us
Remote root: /users/zcy05331/tree-h1-final-20260920T2105Z
Running session: tree-h1-final-matrix
Core binary source: eac95e85285ea08a5d424a6a88c173ad31af2397
Main cleanup was merged and pushed at 6d37533fa66d9471a27fd7f237370113c5e10743.
Those revisions have identical core C++ source. Source changes after eac95e8
only make test dependency paths explicit and update documentation.

## Fixed policy

Use h=1, original gamma/alpha/beta indexing, and P<=6. Maximize coefficient
payload utilization, then minimize P among ties; optimize N0/b afterwards.
For 32-byte entries under t4099/c12/n2048 the layout is g8/P3.
The matrix has 256/512 MiB, 1 GiB and 4 GiB (heights22/23/24/26).
Fifty measured queries per normal row, eight for Cuckoo/TreePIR/Flat,
three warmups, seed7. N0 pilots have eight measured queries.
The seven baselines are StandardOnionPIR, SPIRIT+RC, Layerwise tuned,
Cuckoo batch tuned, TreePIR, Flat and Respire.

## Progress and verification

- Clemson was stopped at an idle boundary and remains paused by user request.
  Do not resume its queue. Prior results remain on that host and in the archive.
- Utah native default48/48 and EP3 primary/noise checks passed.
- Utah SPIRIT+RC: three CTest targets and seven wrapper tests passed.
- Utah Respire: seven tests, including real H8 encrypted retrieval, passed.
- Local integrated regression:48 C++ and89 Python tests, no skips.
- Fresh Utah Standard1GiB:50/50correct; mean full server time 805.180527ms.
- The primary matrix is running. `metadata/primary-progress.json` is a local
  snapshot; query the remote same-named file for current state. Do not resubmit
  completed cases or reuse Clemson performance numbers for Utah.

## Remaining queue

Primary fixed-layout N0 pilots and both query buffers at50trials, then core
baselines, TreePIR backend preparation/tuning, and SPIRIT+RC/Respire formal rows.
Each heavy operation waits for an idle host. Standard1GiB is reused only from
this same run's speed-reference directory. No recurring app monitor is enabled.

Respire4GiB is admitted only if memory fits: its retained database alone is
196GiB, exceeding this machine's125GiB. Record `skipped_resource`; do not call
it a completed performance point. The smaller three Respire sizes remain queued.

Archived source and results: codex/archive-full-pir-20260920 at fcc521f.
Retired algorithms/campaign code were removed from the main tree. Private docs
and paper drafts were excluded from all uploads and commits.
