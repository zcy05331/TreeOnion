# Wider N0 Structure Check

Input buckets come from real-selector-check.csv. Layouts are computed by the current linked PirParams::with_layout, make_benchmark_reference, select_target_plaintext_layout, and compute_layer_layout_features.

| H | variant | N0 | padded | padding delta | sum Nrest | sum Nrest delta | CMux delta | substitutions delta | query NTT delta | physical bytes delta |
|---:|---|---:|---:|---:|---:|---:|---:|---:|---:|---:|
| 24 | baseline_selector | 64 | 1021568 | 0 | 15962 | 0 | 0 | 0 | 0 | 0 |
| 24 | forced_h7 | 64 | 1021568 | 0 | 15962 | 0 | 0 | 0 | 0 | 0 |
| 24 | forced_h8 | 128 | 1022976 | 1408 | 7992 | -7970 | -7970 | 2146 | 4736 | 23068672 |
| 24 | forced_h9 | 256 | 1022976 | 1408 | 3996 | -11966 | -11966 | 6771 | 14208 | 23068672 |
| 24 | forced_h10 | 512 | 1022976 | 1408 | 1998 | -13964 | -13964 | 16058 | 33152 | 23068672 |
| 27 | baseline_selector | 32 | 8191040 | 0 | 255970 | 0 | 0 | 0 | 0 | 0 |
| 27 | forced_h7 | 32 | 8191040 | 0 | 255970 | 0 | 0 | 0 | 0 | 0 |
| 27 | forced_h8 | 128 | 8193152 | 2112 | 64009 | -191961 | -191961 | 3654 | 8064 | 34603008 |
| 27 | forced_h9 | 256 | 8194048 | 3008 | 32008 | -223962 | -223962 | 8694 | 18816 | 49283072 |
| 27 | forced_h10 | 512 | 8195072 | 4032 | 16006 | -239964 | -239964 | 19278 | 40320 | 66060288 |
