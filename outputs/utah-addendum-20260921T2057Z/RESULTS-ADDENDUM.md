# Utah addendum results (Sept 21-22 2026)

Host er031.utah.cloudlab.us (Xeon Gold 5512U, 125 GiB), CPU 0 pinned, memory node 0. Fifty measured queries per row (eight for Cuckoo, TreePIR and Flat), three warmups, seed 7. Logical database = full tree, (2^(H+1)-1) x 32 B.

## Throughput (MB/s = logical bytes / mean server time per complete path)

| Scheme | 256 MiB | 512 MiB | 1 GiB | 2 GiB | 4 GiB |
|---|---:|---:|---:|---:|---:|
| Ours h1 g8/P3 | 769 | 952 | 1120 | 1220 | 1273 |
| Standard OnionPIR (single block) | 985 | 1163 | 1317 | 1416 | 1458 |
| Layerwise tuned | 503 | 666 | 859 | 1033 | 1196 |
| Layerwise + response merging (64-slot strided) | 389 | 522 | 656 | 762 | 835 |
| SPIRIT+RC | 270 | 515 | 923 | 1518 | — |
| Cuckoo batch tuned | 136 | 187 | 240 | 297 | 358 |
| TreePIR | 132 | 162 | 196 | 223 | 177 |
| Flat | 45 | 52 | 54 | 57 | 56 |
| Respire | 19 | 30 | 42 | 62 | — |

## Marginal throughput (MB/s between adjacent sizes, and the implied ceiling)

| Scheme | 256-512 MiB | 512 MiB-1 GiB | 1-2 GiB | 2-4 GiB | ceiling |
|---|---:|---:|---:|---:|---:|
| Ours h1 g8/P3 | 1247 | 1360 | 1341 | 1330 | 1330 |
| Standard OnionPIR (single block) | 1420 | 1518 | 1531 | 1503 | 1503 |
| Layerwise tuned | 986 | 1208 | 1296 | 1421 | 1421 |
| Layerwise + response merging (64-slot strided) | 791 | 883 | 909 | 924 | 924 |
| SPIRIT+RC | 5620 | 4451 | 4274 | — | 4274 |
| Cuckoo batch tuned | 303 | 333 | 390 | 452 | 452 |
| TreePIR | 211 | 248 | 259 | 146 | 146 |
| Flat | 62 | 56 | 61 | 56 | 56 |
| Respire | 83 | 67 | 120 | — | 120 |

A scheme's own curve is flat in this table; a jump means the row changed backend (TreePIR switches from vbpir to sealpir_d2 at 4 GiB). SPIRIT+RC has 3.2 times our marginal throughput and about 5.7 times our fixed cost, which is why it crosses over near 1.4 GiB.


## Server time per path (ms)

| Scheme | 256 MiB | 512 MiB | 1 GiB | 2 GiB | 4 GiB |
|---|---:|---:|---:|---:|---:|
| Ours h1 g8/P3 | 348.9 | 564.2 | 958.8 | 1759.5 | 3373.8 |
| Standard OnionPIR (single block) | 272.6 | 461.7 | 815.2 | 1516.7 | 2945.7 |
| Layerwise tuned | 533.7 | 806.0 | 1250.3 | 2078.9 | 3590.5 |
| Layerwise + response merging (64-slot strided) | 689.5 | 1028.8 | 1637.1 | 2818.8 | 5142.7 |
| SPIRIT+RC | 995.3 | 1043.1 | 1163.7 | 1414.9 | — |
| Cuckoo batch tuned | 1979.3 | 2866.0 | 4479.7 | 7235.7 | 11990.3 |
| TreePIR | 2033.3 | 3306.0 | 5467.3 | 9609.2 | 24321.2 |
| Flat | 5983.3 | 10305.0 | 19849.4 | 37421.8 | 76027.8 |
| Respire | 14497.0 | 17715.7 | 25712.7 | 34688.1 | — |

## Per-stage throughput at 1 GiB (MB/s = logical bytes / stage time)

| Stage | Ours | Standard | Layerwise | Cuckoo | Respire |
|---|---:|---:|---:|---:|---:|
| query expansion | 14434 | 13071 | 2845 | 676 | 103 |
| first dimension | 1460 | 1669 | 1474 | 454 | 181 |
| RGSW fold / other dimensions | 19544 | 12074 | 7574 | 2064 | 121 |
| whole path | 1120 | 1317 | 859 | 240 | 42 |

Ours and Standard are capped by the first dimension alone; every other stage runs above 12 GB/s. Cuckoo pays expansion and first dimension once per bucket (37 buckets at 1 GiB). Respire runs every stage below 200 MB/s, with expansion and fold slower than its own first dimension, because each of its 49 buckets pays a full expansion and a full 256-to-1 fold.


## What response merging costs Layerwise

| Size | Layerwise MB/s | with merging MB/s | ratio | scanned bytes ratio | response |
|---|---:|---:|---:|---:|---:|
| 256 MiB | 503 | 389 | 0.77 | 1.49 | 14.9 KB |
| 512 MiB | 666 | 522 | 0.78 | 1.49 | 14.9 KB |
| 1 GiB | 859 | 656 | 0.76 | 1.49 | 14.9 KB |
| 2 GiB | 1033 | 762 | 0.74 | 1.49 | 14.9 KB |
| 4 GiB | 1196 | 835 | 0.70 | 1.49 | 14.9 KB |

The throughput ratio tracks the inverse of the scanned-byte ratio, so the merge itself is nearly free and the whole cost is the strided layout holding 64 instead of 96 nodes per plaintext. Marginal throughput falls from 1421 to 924 MB/s.


## Communication

Split into what a client uploads once per session (offline) and what one path retrieval costs (online). SPIRIT+RC's adapter charges its reusable key material to every query; it is separated here using the adapter's own per-sample breakdown.

### Offline, once per session

| Scheme | 256 MiB | 512 MiB | 1 GiB | 2 GiB | 4 GiB | Kind |
|---|---:|---:|---:|---:|---:|---|
| Ours h1 g8/P3 | 1.53 MB | 1.53 MB | 1.53 MB | 1.53 MB | 1.53 MB | BV Galois + RGSW(s) keys, seeded model |
| Standard OnionPIR (single block) | 1.42 MB | 1.42 MB | 1.42 MB | 1.42 MB | 1.42 MB | BV Galois + RGSW(s) keys, seeded model |
| Layerwise tuned | 1.42 MB | 1.42 MB | 1.42 MB | 1.42 MB | 1.42 MB | same key bundle shared by every level |
| Layerwise + response merging (64-slot strided) | 1.42 MB | 1.42 MB | 1.42 MB | 1.42 MB | 1.42 MB | same key bundle, also covers the merge automorphisms |
| SPIRIT+RC | 3.30 MB | 3.30 MB | 3.30 MB | 3.30 MB | — | SPIRIT Galois + RC selector/merge keys, actual bytes (see notes) |
| Cuckoo batch tuned | 1.42 MB | 1.42 MB | 1.42 MB | 1.42 MB | 1.42 MB | same key bundle shared by every bucket |
| TreePIR | 37.57 MB | 37.57 MB | 37.57 MB | 37.57 MB | 846.77 MB | GaloisKeys + RelinKeys, actual uncompressed bytes |
| Flat | 1.42 MB | 1.42 MB | 1.42 MB | 1.42 MB | 1.42 MB | same key bundle |
| Respire | 3.94 MB | 3.94 MB | 3.94 MB | 3.94 MB | — | public parameters, crate formula (see notes) |

### Online query, per path

| Scheme | 256 MiB | 512 MiB | 1 GiB | 2 GiB | 4 GiB |
|---|---:|---:|---:|---:|---:|
| Ours h1 g8/P3 | 14.5 KB | 14.5 KB | 14.5 KB | 14.5 KB | 14.5 KB |
| Standard OnionPIR (single block) | 14.5 KB | 14.5 KB | 14.5 KB | 14.5 KB | 14.5 KB |
| Layerwise tuned | 232.5 KB | 247.0 KB | 261.6 KB | 276.1 KB | 290.6 KB |
| Layerwise + response merging (64-slot strided) | 247.0 KB | 261.6 KB | 276.1 KB | 290.6 KB | 305.2 KB |
| SPIRIT+RC | 1.59 MB | 3.09 MB | 6.09 MB | 12.09 MB | — |
| Cuckoo batch tuned | 494.1 KB | 523.1 KB | 537.7 KB | 566.7 KB | 581.2 KB |
| TreePIR | 1.13 MB | 1.13 MB | 1.13 MB | 1.13 MB | 26.01 MB |
| Flat | 319.7 KB | 334.2 KB | 348.8 KB | 363.3 KB | 377.8 KB |
| Respire | 75.0 KB | 117.9 KB | 120.6 KB | 206.3 KB | — |

### Online response, per path

| Scheme | 256 MiB | 512 MiB | 1 GiB | 2 GiB | 4 GiB |
|---|---:|---:|---:|---:|---:|
| Ours h1 g8/P3 | 11.0 KB | 11.0 KB | 11.0 KB | 11.0 KB | 11.0 KB |
| Standard OnionPIR (single block) | 11.0 KB | 11.0 KB | 11.0 KB | 11.0 KB | 11.0 KB |
| Layerwise tuned | 179.9 KB | 190.9 KB | 201.9 KB | 212.9 KB | 223.9 KB |
| Layerwise + response merging (64-slot strided) | 14.9 KB | 14.9 KB | 14.9 KB | 14.9 KB | 14.9 KB |
| SPIRIT+RC | 32.1 KB | 32.1 KB | 32.1 KB | 32.1 KB | — |
| Cuckoo batch tuned | 374.0 KB | 396.0 KB | 407.0 KB | 429.0 KB | 440.0 KB |
| TreePIR | 128.1 KB | 128.1 KB | 128.1 KB | 128.1 KB | 19.52 MB |
| Flat | 242.0 KB | 253.0 KB | 264.0 KB | 275.0 KB | 286.0 KB |
| Respire | 8.0 KB | 8.0 KB | 8.0 KB | 8.0 KB | — |

### Online total, per path

| Scheme | 256 MiB | 512 MiB | 1 GiB | 2 GiB | 4 GiB |
|---|---:|---:|---:|---:|---:|
| Ours h1 g8/P3 | 25.5 KB | 25.5 KB | 25.5 KB | 25.5 KB | 25.5 KB |
| Standard OnionPIR (single block) | 25.5 KB | 25.5 KB | 25.5 KB | 25.5 KB | 25.5 KB |
| Layerwise tuned | 412.4 KB | 438.0 KB | 463.5 KB | 489.0 KB | 514.6 KB |
| Layerwise + response merging (64-slot strided) | 262.0 KB | 276.5 KB | 291.0 KB | 305.6 KB | 320.1 KB |
| SPIRIT+RC | 1.63 MB | 3.13 MB | 6.13 MB | 12.13 MB | — |
| Cuckoo batch tuned | 868.1 KB | 919.1 KB | 944.7 KB | 995.7 KB | 1021.2 KB |
| TreePIR | 1.25 MB | 1.25 MB | 1.25 MB | 1.25 MB | 45.52 MB |
| Flat | 561.7 KB | 587.2 KB | 612.8 KB | 638.3 KB | 663.8 KB |
| Respire | 83.0 KB | 125.9 KB | 128.6 KB | 214.3 KB | — |

Measurement kinds. Query bytes for Ours, Standard, Layerwise, Flat and Cuckoo are the seed-compressed protocol model, one 14.5 KB packed BFV query per PIR call; those schemes upload only keys offline and the server keeps its own preprocessed database. TreePIR and SPIRIT+RC report actual serialized bytes. Respire reports its parameter model. Response bytes are actual serialized bytes for every scheme.

SPIRIT+RC. The adapter counts the RC selector expansion keys, the merge Galois keys and SPIRIT's own Galois keys inside every query. Its per-sample breakdown separates them: the key material is 3.30 MB at every size (SPIRIT Galois 657 KB, RC selector keys 2.38 MB, RC merge Galois 401 KB) and is listed as offline above, leaving the LWE query vector plus the packed secret and the RC selector ciphertext as the online query. Two further offline terms are not in the table. The client hint that its SimplePIR-style first dimension needs has no serializer in the adapter, so it is unmeasured. Its public metadata, the PBC routing tables and bucket layouts, is 0.94, 1.88, 3.75 and 7.50 GiB at the four sizes, reported by the adapter as an in-memory model rather than a wire requirement because it is reconstructible from public seeds and the layout.

Respire. The adapter hardcodes zero offline bytes, but the crate's own `params_public_param_size` charges automorphism, RLWE-to-GSW and scalar-to-vector public parameters: (11 x 24 + 2 x 4 + 8 x 2) elements x 14336 B = 3.94 MiB, plus a compression term of a few hundred KB, independent of database size. That figure is derived from the formula, not measured here, and the crate notes its randomness compression is not actually implemented.

TreePIR. Its 4 GiB row uses the SealPIR backend because every VBPIR candidate ran out of memory, which is why its keys jump from 37.57 MB to 846.77 MB and its query and response jump with them.

## Provenance

Three run roots on the same host, same source lineage, one leaf schedule per size (seed 7):

- `tree-h1-final-20260920T2105Z` (commit eac95e8): TreePIR and the external baselines at 256 MiB, 512 MiB, 1 GiB, 4 GiB.
- `tree-h1-addendum-20260921T2057Z` (commit ae8ae66): every Ours row re-measured on one binary, the 2 GiB point, Standard re-measured at 256/512 MiB under the pinned geometry and re-measured same-day at 1/4 GiB, TreePIR and the external baselines at 2 GiB.
- `tree-h1-addendum2-20260922T0143Z` (commit 8e3e5f9): Layerwise, Cuckoo and Flat re-measured at 256/512 MiB under the pinned geometry, and the merged-response Layerwise variant at all five sizes.

Raw per-round JSON stays on the local sync of those roots and is not committed.

## Findings

**Reference geometry.** Below 2^24 leaves `make_benchmark_reference` picked the smallest feasible BFV expansion height (7), giving the flat database a 32- or 64-wide first dimension; from 2^24 leaves it picks height 10 (first dimension 512). Every Merkle-derived case inherited that envelope, so the small-size columns were not comparable with the large ones. Re-measured at height 10, the small sizes gain: Standard 348 to 985 MB/s at 256 MiB and 353 to 1163 at 512 MiB, Flat 16 to 45 and 15 to 52, Layerwise 407 to 503 and 380 to 666, Cuckoo 131 to 136 and 157 to 187 (its pilot range also widens from height 7 only to 7-10, and it now selects 8). First-dimension scan rate is 1.80-1.87 GB/s at every size afterwards.

**Layerwise still falls off at small sizes** even under one geometry (503 MB/s at 256 MiB against 1196 at 4 GiB): its H independent packed queries cost a fixed 148 ms of expansion plus 74 ms of selector completion at 256 MiB, against 63 ms of expansion total for the single packed tree query.

**Response merging applied to Layerwise** (strided 64-slot level plaintexts, one shared 6-bit gamma selector set, `mlwe_merge_residues`) cuts the response from one ciphertext per level to one ciphertext for the whole path: 180-224 KB down to 14.9 KB, within 36 percent of the tree scheme's 11.0 KB. Rotation and merge are cheap (13.6 and 2.0 ms at 256 MiB). The cost is the strided layout's 64 instead of 96 nodes per plaintext, which inflates the scanned database by 1.5x and costs 23-30 percent of throughput at every size. The tree scheme avoids that trade because its 3-plane codec reaches 88.9 percent coefficient utilization while keeping the stride.

**Respire, same RLWE-RGSW paradigm, 20x slower than our Cuckoo baseline** at 1 GiB. Its own stage timers: query expansion 10.5 s (41 percent), first dimension 5.9 s (23 percent), GSW fold 8.9 s (34 percent). Three causes, in order of size: its 49 cuckoo buckets each pay a full query expansion and a full 256-to-1 GSW fold (214 and 181 ms per bucket, against 43 and 14 ms per bucket for our Cuckoo baseline, and one 63 ms expansion for the tree scheme, which then serves all levels); its 4-bit plaintext modulus needs three times the coefficients per database byte, giving 0.55 GB/s of plaintext in the first dimension against 1.55 GB/s; and its AVX2 u64 kernels against HEXL AVX-512 u32 CRT kernels. What it buys is the smallest communication in the table: 8 KB responses, 75-206 KB queries, no helper keys.

**SPIRIT+RC crosses over near 1.4 GiB.** Its server time is 913 ms of fixed cost plus about 251 ms per GiB; ours is about 160 ms plus 800 ms per GiB. The fixed part is RC selector expansion (513 ms), hint query precompute (203 ms) and LWE-to-RLWE packing (138 ms); the marginal part is a plaintext-byte LWE matrix-vector product, which streams half the bytes we do at higher bandwidth. It pays for that with queries that grow to 15.4 MB, 38.8 s of client time per path, 76 GiB resident at 2 GiB (out of memory at 4 GiB), and a parameter line SEAL only accepts at `sec_level=none`.

**Scan-buffer reuse** (one scratch per response instead of two allocations per level and plane) was measured by alternating the old and new binaries on the same host: 1.0-1.5 percent of scan core at 1 GiB, about 2.4 percent at 4 GiB after excluding a host disturbance. Ours still scans 6-9 percent slower per byte than Standard on same-day rows (9.06-9.28 against 9.83-9.93 GB/s of physical bytes); that gap is unexplained. The 12.5 percent extra bytes from the g8/P3 layout remain the larger term.

## Row notes

- Ours h1 g8/P3: 256mib: N0 256, split-columns; 512mib: N0 512, interleaved; 1gib: N0 512, split-columns; 2gib: N0 512, interleaved; 4gib: N0 512, interleaved
- Standard OnionPIR (single block): 256mib: h10 N0 512; 512mib: h10 N0 512; 1gib: h10 N0 512; 2gib: h10 N0 512; 4gib: h10 N0 512
- Layerwise tuned: 256mib: 16 calls, h10; 512mib: 17 calls, h10; 1gib: 18 calls, h10; 2gib: 19 calls, h10; 4gib: 20 calls, h10
- Layerwise + response merging (64-slot strided): 256mib: 17 queries, resp 15296 B; 512mib: 18 queries, resp 15296 B; 1gib: 19 queries, resp 15296 B; 2gib: 20 queries, resp 15296 B; 4gib: 21 queries, resp 15296 B
- Cuckoo batch tuned: 256mib: 34 buckets, h8; 512mib: 36 buckets, h8; 1gib: 37 buckets, h9; 2gib: 39 buckets, h9; 4gib: 40 buckets, h10
- TreePIR: 256mib: vbpir_fd256; 512mib: vbpir_fd256; 1gib: vbpir_fd256; 2gib: vbpir_fd256; 4gib: sealpir_d2
- Flat: 256mib: 22 calls, h10; 512mib: 23 calls, h10; 1gib: 24 calls, h10; 2gib: 25 calls, h10; 4gib: 26 calls, h10
