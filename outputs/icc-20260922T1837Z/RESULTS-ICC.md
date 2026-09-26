# Campus Cluster results (Sept 22-25 2026)

Nodes ccc0400-ccc0415 (Xeon Platinum 8358, 2 x 32 cores, 251 GiB), whole-node exclusive jobs, one thread pinned to CPU 0 with memory on node 0; every job checks the node configuration and repeats a Standard 1 GiB calibration row. Seed-7 leaf schedule; logical database = full tree, (2^(H+1)-1) x 32 B; throughput = logical bytes / mean server time per complete path. Ours and the OnionPIRv2 baselines run the ep3-s32 selector gadget, built on each job's node.

## Findings

- **Ours against Layerwise tuned**, both on ep3-s32: 1.31x, 1.26x, 1.20x, 1.11x, 1.04x, 0.94x from 256 MiB to 8 GiB. Layerwise is faster at 8 GiB; it makes 21 calls and sends 540.1 KB online per path against our 25.5 KB. The two 8 GiB jobs ran on different nodes whose calibration rows differ by 0.2%.
- **Node consistency**: the Standard 1 GiB calibration row spans 807.8–817.5 ms (1.2%) over 28 jobs on 7 nodes.
- **Ours against Standard OnionPIR** (one block, a speed reference, not a path protocol): 0.71x, 0.71x, 0.75x, 0.80x, 0.83x, 0.83x from 256 MiB to 8 GiB.
- **Selector gadget**: moving the OnionPIRv2 baselines from exact EP6 to ep3-s32 raises them by Standard OnionPIR +6% to +8%; Layerwise tuned +9% to +21%; Layerwise + response merging +9% to +19%; Cuckoo batch tuned +6% to +19%; Flat +6% to +8% (256 MiB to 4 GiB). Ours ran ep3-s32 in both runs and moved by -0.8% to +2.0%.
- **Fastest baseline outside the OnionPIRv2 family**: VIA + PBC at 50%–60% of our throughput (421–669 MB/s), with 12.57 MB–23.79 MB online per path and no offline keys.
- **Lowest online communication**: VIA-B, 15.7 KB–22.2 KB per path after 6.36 MB of offline keys, at 27–47 MB/s. Ours sends 25.5 KB at every size after 1.53 MB of keys.
- **TreePIR + Spiral** (TreePIR's colors, indices and leaves, Spiral per color): 19–123 MB/s, 0.31x–0.89x the throughput of TreePIR on SealPIR d=2 and 1.8x–2.4x that of Spiral + PBC. Spiral's query expansion, paid once per color, is 91% of its server time at 256 MiB and 31% at 8 GiB. It cuts TreePIR's keys from 716.50 MB–879.34 MB to 6.00 MB–7.75 MB and its online traffic from 38.52 MB–47.27 MB to 792.7 KB–1.48 MB per path.
- **Ablation**: the merged Layerwise step reaches 59%–72% of our throughput, and packing its queries cuts its online traffic from 262.0 KB–334.6 KB to 29.5 KB–44.0 KB at the same speed (0.99x–1.03x).

## Throughput (MB/s)

| Scheme | 256 MiB | 512 MiB | 1 GiB | 2 GiB | 4 GiB | 8 GiB |
|---|---:|---:|---:|---:|---:|---:|
| Ours h1 g8/P3 | 699 | 881 | 1076 | 1231 | 1333 | 1345 |
| Standard OnionPIR (single block) | 979 | 1246 | 1427 | 1536 | 1610 | 1630 |
| Layerwise tuned | 534 | 697 | 894 | 1109 | 1287 | 1427 |
| Cuckoo batch tuned | 137 | 195 | 240 | 304 | 368 | 372 |
| Spiral + PBC | 10 | 17 | 23 | 32 | 41 | over memory |
| KsPIR + PBC | 149 | 183 | 217 | 252 | 278 | 243 |
| VIA-B | 27 | 35 | 40 | 44 | 46 | 47 |
| VIA + PBC | 421 | 489 | 569 | 624 | 665 | 669 |
| TreePIR | 59 | 73 | 91 | 107 | 125 | 138 |
| TreePIR + Spiral | 19 | 30 | 49 | 69 | 97 | 123 |
| Flat | 46 | 54 | 59 | 61 | 61 | 60 |
| Respire | 16 | 26 | 37 | 56 | 66 | over memory |

## Server time per path (ms)

| Scheme | 256 MiB | 512 MiB | 1 GiB | 2 GiB | 4 GiB | 8 GiB |
|---|---:|---:|---:|---:|---:|---:|
| Ours h1 g8/P3 | 384.1 | 609.0 | 998.2 | 1744.6 | 3221.3 | 6387.3 |
| Standard OnionPIR (single block) | 274.1 | 430.9 | 752.5 | 1398.1 | 2667.0 | 5270.0 |
| Layerwise tuned | 502.6 | 770.0 | 1200.9 | 1937.2 | 3336.9 | 6021.2 |
| Cuckoo batch tuned | 1959.1 | 2758.0 | 4475.3 | 7064.0 | 11681.1 | 23082.6 |
| Spiral + PBC | 26091.1 | 31698.1 | 45976.7 | 67423.0 | 103958.8 | over memory |
| KsPIR + PBC | 1798.9 | 2931.9 | 4942.5 | 8515.8 | 15452.1 | 35382.1 |
| VIA-B | 9975.2 | 15297.4 | 26577.3 | 48259.0 | 93224.9 | 183319.8 |
| VIA + PBC | 637.9 | 1097.6 | 1887.5 | 3441.9 | 6453.9 | 12837.0 |
| TreePIR | 4540.3 | 7364.5 | 11771.2 | 20113.7 | 34479.4 | 62251.8 |
| TreePIR + Spiral | 14502.9 | 17652.6 | 21726.5 | 31029.6 | 44123.1 | 70009.9 |
| Flat | 5892.8 | 9899.3 | 18132.4 | 35024.0 | 69953.3 | 142574.8 |
| Respire | 17170.1 | 20476.9 | 29010.3 | 38300.1 | 65307.6 | over memory |

## Ablation: the primary algorithm's steps on the Layerwise baseline (MB/s)

| Scheme | 256 MiB | 512 MiB | 1 GiB | 2 GiB | 4 GiB | 8 GiB |
|---|---:|---:|---:|---:|---:|---:|
| Layerwise tuned | 534 | 697 | 894 | 1109 | 1287 | 1427 |
| Layerwise + response merging | 410 | 539 | 682 | 817 | 921 | 975 |
| Layerwise + merging + packed query | 412 | 542 | 679 | 813 | 922 | 999 |
| Ours h1 g8/P3 | 699 | 881 | 1076 | 1231 | 1333 | 1345 |

Layerwise + response merging adds the primary algorithm's rotation and MLWE merge on the transposed layout with one shared gamma selector; the packed variant also packs every level's query into shared expansion slots. Both reuse the primary algorithm's selectors, so they are ablation steps, not baselines.

## Selector gadget (MB/s, exact EP6 / ep3-s32)

| Scheme | 256 MiB | 512 MiB | 1 GiB | 2 GiB | 4 GiB |
|---|---:|---:|---:|---:|---:|
| Ours h1 g8/P3 | 694 / 699 | 881 / 881 | 1085 / 1076 | 1215 / 1231 | 1307 / 1333 |
| Standard OnionPIR (single block) | 927 / 979 | 1158 / 1246 | 1323 / 1427 | 1435 / 1536 | 1499 / 1610 |
| Layerwise tuned | 440 / 534 | 601 / 697 | 789 / 894 | 997 / 1109 | 1176 / 1287 |
| Layerwise + response merging | 345 / 410 | 468 / 539 | 606 / 682 | 736 / 817 | 846 / 921 |
| Cuckoo batch tuned | 115 / 137 | 163 / 195 | 210 / 240 | 271 / 304 | 348 / 368 |
| Flat | 42 / 46 | 50 / 54 | 56 / 59 | 58 / 61 | 57 / 61 |

The Ours row compares its first run with the rerun from the same build as the baselines; both use ep3-s32.

## Communication


### Offline, once per session

| Scheme | 256 MiB | 512 MiB | 1 GiB | 2 GiB | 4 GiB | 8 GiB |
|---|---:|---:|---:|---:|---:|---:|
| Ours h1 g8/P3 | 1.53 MB | 1.53 MB | 1.53 MB | 1.53 MB | 1.53 MB | 1.53 MB |
| Standard OnionPIR (single block) | 1.42 MB | 1.42 MB | 1.42 MB | 1.42 MB | 1.42 MB | 1.42 MB |
| Layerwise tuned | 1.42 MB | 1.42 MB | 1.42 MB | 1.42 MB | 1.42 MB | 1.42 MB |
| Cuckoo batch tuned | 1.42 MB | 1.42 MB | 1.42 MB | 1.42 MB | 1.42 MB | 1.42 MB |
| Spiral + PBC | 6.88 MB | 6.88 MB | 7.75 MB | 7.75 MB | 7.75 MB | over memory |
| KsPIR + PBC | 5.55 MB | 5.65 MB | 8.63 MB | 8.72 MB | 8.82 MB | 15.99 MB |
| VIA-B | 6.36 MB | 6.36 MB | 6.36 MB | 6.36 MB | 6.36 MB | 6.36 MB |
| VIA + PBC | none | none | none | none | none | none |
| TreePIR | 716.50 MB | 749.07 MB | 781.64 MB | 814.20 MB | 846.77 MB | 879.34 MB |
| TreePIR + Spiral | 6.00 MB | 6.88 MB | 6.88 MB | 7.75 MB | 7.75 MB | 7.75 MB |
| Flat | 1.42 MB | 1.42 MB | 1.42 MB | 1.42 MB | 1.42 MB | 1.42 MB |
| Respire | 3.94 MB | 3.94 MB | 3.94 MB | 3.94 MB | 3.94 MB | over memory |
| Layerwise + response merging | 1.42 MB | 1.42 MB | 1.42 MB | 1.42 MB | 1.42 MB | 1.42 MB |
| Layerwise + merging + packed query | 1.53 MB | 1.53 MB | 1.53 MB | 1.53 MB | 1.53 MB | 1.53 MB |

### Online query, per path

| Scheme | 256 MiB | 512 MiB | 1 GiB | 2 GiB | 4 GiB | 8 GiB |
|---|---:|---:|---:|---:|---:|---:|
| Ours h1 g8/P3 | 14.5 KB | 14.5 KB | 14.5 KB | 14.5 KB | 14.5 KB | 14.5 KB |
| Standard OnionPIR (single block) | 14.5 KB | 14.5 KB | 14.5 KB | 14.5 KB | 14.5 KB | 14.5 KB |
| Layerwise tuned | 232.5 KB | 247.0 KB | 261.6 KB | 276.1 KB | 290.6 KB | 305.2 KB |
| Cuckoo batch tuned | 494.1 KB | 523.1 KB | 537.7 KB | 566.7 KB | 581.2 KB | 610.3 KB |
| Spiral + PBC | 545.1 KB | 577.1 KB | 593.2 KB | 625.2 KB | 641.2 KB | over memory |
| KsPIR + PBC | 5.05 MB | 5.35 MB | 5.50 MB | 5.80 MB | 5.94 MB | 6.24 MB |
| VIA-B | 9.3 KB | 10.1 KB | 11.0 KB | 11.9 KB | 12.9 KB | 13.9 KB |
| VIA + PBC | 12.04 MB | 14.75 MB | 15.71 MB | 17.14 MB | 19.81 MB | 23.13 MB |
| TreePIR | 22.00 MB | 23.00 MB | 24.01 MB | 25.01 MB | 26.01 MB | 27.01 MB |
| TreePIR + Spiral | 352.7 KB | 368.7 KB | 384.8 KB | 400.8 KB | 416.8 KB | 432.8 KB |
| Flat | 319.7 KB | 334.2 KB | 348.8 KB | 363.3 KB | 377.8 KB | 392.3 KB |
| Respire | 75.0 KB | 117.9 KB | 120.6 KB | 206.3 KB | 209.0 KB | over memory |
| Layerwise + response merging | 247.0 KB | 261.6 KB | 276.1 KB | 290.6 KB | 305.2 KB | 319.7 KB |
| Layerwise + merging + packed query | 14.5 KB | 14.5 KB | 29.1 KB | 29.1 KB | 29.1 KB | 29.1 KB |

### Online response, per path

| Scheme | 256 MiB | 512 MiB | 1 GiB | 2 GiB | 4 GiB | 8 GiB |
|---|---:|---:|---:|---:|---:|---:|
| Ours h1 g8/P3 | 11.0 KB | 11.0 KB | 11.0 KB | 11.0 KB | 11.0 KB | 11.0 KB |
| Standard OnionPIR (single block) | 11.0 KB | 11.0 KB | 11.0 KB | 11.0 KB | 11.0 KB | 11.0 KB |
| Layerwise tuned | 179.9 KB | 190.9 KB | 201.9 KB | 212.9 KB | 223.9 KB | 234.9 KB |
| Cuckoo batch tuned | 374.0 KB | 396.0 KB | 407.0 KB | 429.0 KB | 440.0 KB | 462.0 KB |
| Spiral + PBC | 680.0 KB | 720.0 KB | 740.0 KB | 780.0 KB | 1.56 MB | over memory |
| KsPIR + PBC | 1.86 MB | 1.97 MB | 2.02 MB | 2.13 MB | 2.19 MB | 2.30 MB |
| VIA-B | 6.4 KB | 6.5 KB | 6.6 KB | 8.1 KB | 8.2 KB | 8.3 KB |
| VIA + PBC | 542.9 KB | 574.9 KB | 590.8 KB | 622.8 KB | 638.8 KB | 670.7 KB |
| TreePIR | 16.51 MB | 17.26 MB | 18.02 MB | 18.77 MB | 19.52 MB | 20.27 MB |
| TreePIR + Spiral | 440.0 KB | 460.0 KB | 480.0 KB | 500.0 KB | 520.0 KB | 1.05 MB |
| Flat | 242.0 KB | 253.0 KB | 264.0 KB | 275.0 KB | 286.0 KB | 297.0 KB |
| Respire | 8.0 KB | 8.0 KB | 8.0 KB | 8.0 KB | 8.0 KB | over memory |
| Layerwise + response merging | 14.9 KB | 14.9 KB | 14.9 KB | 14.9 KB | 14.9 KB | 14.9 KB |
| Layerwise + merging + packed query | 14.9 KB | 14.9 KB | 14.9 KB | 14.9 KB | 14.9 KB | 14.9 KB |

### Online total, per path

| Scheme | 256 MiB | 512 MiB | 1 GiB | 2 GiB | 4 GiB | 8 GiB |
|---|---:|---:|---:|---:|---:|---:|
| Ours h1 g8/P3 | 25.5 KB | 25.5 KB | 25.5 KB | 25.5 KB | 25.5 KB | 25.5 KB |
| Standard OnionPIR (single block) | 25.5 KB | 25.5 KB | 25.5 KB | 25.5 KB | 25.5 KB | 25.5 KB |
| Layerwise tuned | 412.4 KB | 438.0 KB | 463.5 KB | 489.0 KB | 514.6 KB | 540.1 KB |
| Cuckoo batch tuned | 868.1 KB | 919.1 KB | 944.7 KB | 995.7 KB | 1021.2 KB | 1.05 MB |
| Spiral + PBC | 1.20 MB | 1.27 MB | 1.30 MB | 1.37 MB | 2.19 MB | over memory |
| KsPIR + PBC | 6.91 MB | 7.32 MB | 7.52 MB | 7.93 MB | 8.13 MB | 8.54 MB |
| VIA-B | 15.7 KB | 16.6 KB | 17.6 KB | 20.0 KB | 21.1 KB | 22.2 KB |
| VIA + PBC | 12.57 MB | 15.31 MB | 16.29 MB | 17.75 MB | 20.43 MB | 23.79 MB |
| TreePIR | 38.52 MB | 40.27 MB | 42.02 MB | 43.77 MB | 45.52 MB | 47.27 MB |
| TreePIR + Spiral | 792.7 KB | 828.7 KB | 864.8 KB | 900.8 KB | 936.8 KB | 1.48 MB |
| Flat | 561.7 KB | 587.2 KB | 612.8 KB | 638.3 KB | 663.8 KB | 689.3 KB |
| Respire | 83.0 KB | 125.9 KB | 128.6 KB | 214.3 KB | 217.0 KB | over memory |
| Layerwise + response merging | 262.0 KB | 276.5 KB | 291.0 KB | 305.6 KB | 320.1 KB | 334.6 KB |
| Layerwise + merging + packed query | 29.5 KB | 29.5 KB | 44.0 KB | 44.0 KB | 44.0 KB | 44.0 KB |

## Against the Utah run, same gadget (ICC MB/s / Utah MB/s)

Ours from its first run and the OnionPIRv2 baselines from the exact-EP6 runs, as at Utah, so the ratio is the node alone. Utah's TreePIR used VBPIR below 4 GiB; ICC's uses SealPIR d=2 throughout.

| Scheme | 256 MiB | 512 MiB | 1 GiB | 2 GiB | 4 GiB |
|---|---:|---:|---:|---:|---:|
| Ours h1 g8/P3 | 0.90 | 0.93 | 0.97 | 1.00 | 1.03 |
| Standard OnionPIR (single block) | 0.94 | 1.00 | 1.00 | 1.01 | 1.03 |
| Layerwise tuned | 0.87 | 0.90 | 0.92 | 0.96 | 0.98 |
| Layerwise + response merging | 0.89 | 0.90 | 0.92 | 0.97 | 1.01 |
| Cuckoo batch tuned | 0.85 | 0.87 | 0.88 | 0.91 | 0.97 |
| Flat | 0.94 | 0.97 | 1.03 | 1.01 | 1.03 |
| TreePIR | 0.45 | 0.45 | 0.47 | 0.48 | 0.70 |
| Respire | 0.82 | 0.87 | 0.88 | 0.90 | — |

## Calibration and provenance (Standard 1 GiB, one row per job)

| Job | node | source | server ms |
|---|---|---|---:|
| core-cuckoo | ccc0415 | 9accc56 | 808.5 |
| core-cuckoo-8g | ccc0409 | bec158e | 812.8 |
| core-cuckoo-ep3 | ccc0410 | bec158e | 813.5 |
| core-merged | ccc0415 | 9accc56 | 807.8 |
| core-merged-8g | ccc0409 | bec158e | 815.0 |
| core-merged-ep3 | ccc0414 | bec158e | 811.1 |
| core-packed-8g | ccc0412 | bec158e | 813.1 |
| core-packed-ep3 | ccc0414 | bec158e | 812.7 |
| core-slf | ccc0415 | 9accc56 | 810.7 |
| core-slf-8g | ccc0412 | bec158e | 814.3 |
| core-slf-ep3 | ccc0411 | bec158e | 813.8 |
| kspir-pbc | ccc0411 | bec158e | 811.1 |
| kspir-pbc-8g | ccc0414 | bec158e | 812.0 |
| kspir-pbc-failed-10696075 | ccc0414 | bec158e | 813.2 |
| primary | ccc0415 | 9accc56 | 807.9 |
| primary-8g | ccc0414 | bec158e | 813.0 |
| primary-v2 | ccc0414 | bec158e | 812.9 |
| respire-4g | ccc0414 | 1a18b4a | 812.6 |
| respire-small | ccc0415 | ee0b305 | 807.8 |
| spiral-pbc | ccc0414 | bec158e | 816.6 |
| treepir | ccc0415 | ee0b305 | 809.3 |
| treepir-8g | ccc0410 | bec158e | 813.8 |
| treepir-spiral | ccc0408 | a98d4cb | 813.9 |
| treepir-vbpir | ccc0410 | bec158e | 813.3 |
| via-b | ccc0409 | bec158e | 814.6 |
| via-b-8g | ccc0412 | bec158e | 813.0 |
| via-pbc | ccc0411 | bec158e | 812.5 |
| via-pbc-8g | ccc0412 | bec158e | 817.5 |

## Row notes

- Ours h1 g8/P3: 256 MiB: N0 256, split-columns; 512 MiB: N0 512, split-columns; 1 GiB: N0 512, split-columns; 2 GiB: N0 512, split-columns; 4 GiB: N0 512, split-columns; 8 GiB: N0 512, interleaved
- Standard OnionPIR (single block): 256 MiB: 1 call, h10; 512 MiB: 1 call, h10; 1 GiB: 1 call, h10; 2 GiB: 1 call, h10; 4 GiB: 1 call, h10; 8 GiB: 1 call, h10
- Layerwise tuned: 256 MiB: 16 calls, h10; 512 MiB: 17 calls, h10; 1 GiB: 18 calls, h10; 2 GiB: 19 calls, h10; 4 GiB: 20 calls, h10; 8 GiB: 21 calls, h10
- Cuckoo batch tuned: 256 MiB: 34 buckets, h8; 512 MiB: 36 buckets, h8; 1 GiB: 37 buckets, h8; 2 GiB: 39 buckets, h9; 4 GiB: 40 buckets, h10; 8 GiB: 42 buckets, h10
- Spiral + PBC: 256 MiB: nu_1 9, nu_2 3, instances 1; 512 MiB: nu_1 9, nu_2 4, instances 1; 1 GiB: nu_1 9, nu_2 5, instances 1; 2 GiB: nu_1 9, nu_2 6, instances 1; 4 GiB: nu_1 9, nu_2 6, instances 2
- KsPIR + PBC: 256 MiB: r 2, N1 64, icpx; 512 MiB: r 4, N1 64, icpx; 1 GiB: r 8, N1 128, icpx; 2 GiB: r 16, N1 128, icpx; 4 GiB: r 32, N1 128, icpx; 8 GiB: r 64, N1 256, icpx
- VIA-B: 256 MiB: I 256, J 1024, 22 queries, gcc; 512 MiB: I 512, J 1024, 23 queries, gcc; 1 GiB: I 1024, J 1024, 24 queries, gcc; 2 GiB: I 1024, J 2048, 25 queries, gcc; 4 GiB: I 1024, J 4096, 26 queries, gcc; 8 GiB: I 2048, J 4096, 27 queries, gcc
- VIA + PBC: 256 MiB: I 64, J 176, 34 queries, gcc; 512 MiB: I 128, J 167, 36 queries, gcc; 1 GiB: I 128, J 324, 37 queries, gcc; 2 GiB: I 128, J 616, 39 queries, gcc; 4 GiB: I 256, J 600, 40 queries, gcc; 8 GiB: I 512, J 572, 42 queries, icpx
- TreePIR: 256 MiB: sealpir_d2; 512 MiB: sealpir_d2; 1 GiB: sealpir_d2; 2 GiB: sealpir_d2; 4 GiB: sealpir_d2; 8 GiB: sealpir_d2
- TreePIR + Spiral: 256 MiB: nu_1 9, nu_2 2, instances 1; 512 MiB: nu_1 9, nu_2 3, instances 1; 1 GiB: nu_1 9, nu_2 4, instances 1; 2 GiB: nu_1 9, nu_2 5, instances 1; 4 GiB: nu_1 9, nu_2 6, instances 1; 8 GiB: nu_1 9, nu_2 6, instances 2
- Flat: 256 MiB: 22 calls, h10; 512 MiB: 23 calls, h10; 1 GiB: 24 calls, h10; 2 GiB: 25 calls, h10; 4 GiB: 26 calls, h10; 8 GiB: 27 calls, h10
- Layerwise + response merging: 256 MiB: 17 calls, h10; 512 MiB: 18 calls, h10; 1 GiB: 19 calls, h10; 2 GiB: 20 calls, h10; 4 GiB: 21 calls, h10; 8 GiB: 22 calls, h10
- Layerwise + merging + packed query: 256 MiB: 1 call, h10; 512 MiB: 1 call, h10; 1 GiB: 2 calls, h10; 2 GiB: 2 calls, h10; 4 GiB: 2 calls, h10; 8 GiB: 2 calls, h10

## Limitations

- Not measured: Spiral + PBC and Respire at 8 GiB (Spiral + PBC about 336 GiB, Respire about 400 GiB estimated from the 4 GiB peaks, against 251 GiB per node). TreePIR + VBPIR fails with a zero noise budget at 4 and 8 GiB (72 and 138 GiB peak, not memory), so the TreePIR row uses SealPIR d=2 at every size.
- VIA + PBC uses a ternary S2 instead of the paper's sigma-4960 key, which exceeds the CMux bound (scripts/path_baselines/via_pbc/README.md); its largest decoding error was 27–37 of 64 q4 units.
- The first core-slf, core-merged, core-cuckoo and primary runs used exact EP6 for the baselines and a 32-bit-seeded RNG; they appear only in the gadget and Utah comparisons.
