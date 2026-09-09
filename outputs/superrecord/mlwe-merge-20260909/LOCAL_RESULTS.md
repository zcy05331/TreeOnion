# Local Rosetta validation (256 MiB, 32 B, h3)

Same EP2/drop36, KS8 binary; fixed h3/G64/P1/N0=352/b7; 50 measured samples
per order, reversed order in the second round (100 per mode, six warmups each).
All requested paths decoded correctly. Baseline is the current Projection mode.

| Metric | Projection | MLWE |
|---|---:|---:|
| Server time (ms) | 471.813 | 477.896 |
| Response processing: rotate+project+pack+merge (ms) | 19.870 | 11.083 |
| Projection or merge stage (ms) | 12.353 | 3.350 |
| First-dimension scan (ms) | 344.696 | 356.512 |
| Response ciphertext payload (bytes) | 11264 | 11264 |
| Modeled seeded query (bytes) | 14880 | 14880 |

The new merge saves 9.082 ms in the changed stages.
Full server time is 1.29% higher here; this run does not show a full-query speedup.
The unrelated scan stage increased 11.816 ms.
Local Git history cleanup also ran during this comparison, so whole-query results
are diagnostic. A dedicated idle native-node comparison is required for the final
performance assessment. Actual local merge counts: M=23,rho=32 -> 24 KS versus
95 old projection KS. The one-GiB shapes have different H/counts (27 and 79 KS).

Source label records the original feca044 base plus uncommitted MLWE edits; all
source content will be archived by the subsequent implementation commit. History
is being rewritten separately to remove private docs; old commit IDs remain
historical provenance, not public draft links.
