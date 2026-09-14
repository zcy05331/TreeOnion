# New-host 1 GiB parameter comparison

OnionPIR core crypto is fixed at n=2048, logq=58, logt=13.
Layout tuning prioritizes mean complete-path server time.

Layerwise profiling: legacy layer sum 1779.04 ms, selected layer sum
1376.19 ms (22.6% lower), selected padding 350580 vs 349884 plaintexts
(0.20% higher). The selected layout then measured 1368.806644 ms over
100 complete paths. The legacy estimate is not a separate 100-query full-path
measurement. Its largest levels used N0=128/256, selected N0=512/512.

| Cuckoo pilot expansion height | N0 | Mean server ms | Measurements |
|---|---:|---:|---:|
| 7 | 64 | 7484.128314 | 16 |
| 8 | 128 | 5404.106672 | 16 |
| 9 | 256 | 5069.544651 | 16 |
| 10 | 512 | 5964.907601 | 16 |

Selected N0=256; independent formal run: 5083.727627 ms, 16/16 correct.
Every Cuckoo point uses 37 buckets and the same leaf schedule. Query model
550560 B, actual response416768 B, helper model1488000 B are unchanged.

Raw source: profile.json, layerwise_tuned.json, cuckoo/selection.json and
cuckoo/cuckoo_tuned.json. See METADATA_NOTES.md for the static g32 hardware text.
