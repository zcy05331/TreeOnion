# Final result index

See RESULTS.md and SUMMARY.json for final source8506e7f and native-v2 results.
Native-v1 and the initial formal-local-h3 run are retained diagnostics.
The corrected local comparison is formal-local-v2.

# Small-gadget Generic experiment (September 9, 2026)

Implemented source: 5d5f6c39447fa4b4beeaa136846ba2796e25c70d.

Baseline C++ source: a9503fb (same benchmark/alpha code as e425c6f).
The frozen local baseline executable is build-gadget-baseline-local/Onion-PIR.
The new default kernel uses the original EP6/KEY10/KS8 parameters and integer
bit operations for decomposition. Candidate binaries live under build-gadget-configs.

Local platform is x86_64 under Rosetta. The compiler target exposes SSE4.2,
not AVX2/AVX512. Local timings measure this environment, not native CloudLab.

The comparison fixes 32-byte entries and h3/G64/P1/N0=352/b7 at 2^22 leaves
(about 256 MiB). Query expansion height is fixed at 9. Keys use height 11.
KEY remains exact with 10 rows. Noise margins are diagnostic; every requested
plaintext must decrypt correctly. Pilot measurements are screening data only.

Build logs/parameter manifest: builds/.
Default regression and arithmetic evidence: local-validation/.
Full timing results and rejected candidates are retained in separate directories
by scripts/benchmark_gadget_configs.py. Both execution orders use the same leaf
schedule. Expected changes in gadget fields and key sizes are recorded.
