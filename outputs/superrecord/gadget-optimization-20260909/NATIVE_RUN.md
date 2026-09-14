# Initial native validation run (v1)

Host: zcy05331@sm110p-10s10616.wisc.cloudlab.us
Remote root: /users/zcy05331/generic-gadgets-20260909
Tmux session: generic-gadgets-20260909
Source: 5d5f6c39447fa4b4beeaa136846ba2796e25c70d
Baseline: a9503fb, preserved in wisconsin-alpha-entry-20260908/source/build-native.

The local comparison completed first (500 correct measured h3 queries).
The native run tests baseline, kernel-only, EP3/drop32, and EP2/drop36.
Each has two orders of 50 queries, plus three warmups per order.
Both h3 and h1 use 1 GiB, 32 B, 2^24 leaves; CPU0 and interleaved memory.
Fixed h3 layout: 3/64/1/352/9 with expansion9.
Fixed h1 layout: 1/8/3/512/7 with expansion10.
Only actual successful plaintext checks admit a measured candidate.
All previous node experiments were complete before launch.

Raw data: outputs/h3 and outputs/h1 on the remote root.
Progress: metadata/progress.json. Launcher: run_native.py (retained alongside this file).

Final run: /users/zcy05331/generic-gadgets-v2-20260909, source8506e7f.
All final outputs are stored in native-v2/. See RESULTS.md.
