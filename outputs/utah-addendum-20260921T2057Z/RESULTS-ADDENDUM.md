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

## Communication per path

| Scheme | | 256 MiB | 512 MiB | 1 GiB | 2 GiB | 4 GiB |
|---|---|---:|---:|---:|---:|---:|
| Ours h1 g8/P3 | query | 14.5 KB | 14.5 KB | 14.5 KB | 14.5 KB | 14.5 KB |
|  | response | 11.0 KB | 11.0 KB | 11.0 KB | 11.0 KB | 11.0 KB |
|  | helper keys | 1.53 MB | 1.53 MB | 1.53 MB | 1.53 MB | 1.53 MB |
| Standard OnionPIR (single block) | query | 14.5 KB | 14.5 KB | 14.5 KB | 14.5 KB | 14.5 KB |
|  | response | 11.0 KB | 11.0 KB | 11.0 KB | 11.0 KB | 11.0 KB |
|  | helper keys | 1.42 MB | 1.42 MB | 1.42 MB | 1.42 MB | 1.42 MB |
| Layerwise tuned | query | 232.5 KB | 247.0 KB | 261.6 KB | 276.1 KB | 290.6 KB |
|  | response | 179.9 KB | 190.9 KB | 201.9 KB | 212.9 KB | 223.9 KB |
|  | helper keys | 1.42 MB | 1.42 MB | 1.42 MB | 1.42 MB | 1.42 MB |
| Layerwise + response merging (64-slot strided) | query | 247.0 KB | 261.6 KB | 276.1 KB | 290.6 KB | 305.2 KB |
|  | response | 14.9 KB | 14.9 KB | 14.9 KB | 14.9 KB | 14.9 KB |
|  | helper keys | 1.42 MB | 1.42 MB | 1.42 MB | 1.42 MB | 1.42 MB |
| SPIRIT+RC | query | 4.90 MB | 6.40 MB | 9.40 MB | 15.40 MB | — |
|  | response | 32.1 KB | 32.1 KB | 32.1 KB | 32.1 KB | — |
| Cuckoo batch tuned | query | 494.1 KB | 523.1 KB | 537.7 KB | 566.7 KB | 581.2 KB |
|  | response | 374.0 KB | 396.0 KB | 407.0 KB | 429.0 KB | 440.0 KB |
|  | helper keys | 1.42 MB | 1.42 MB | 1.42 MB | 1.42 MB | 1.42 MB |
| TreePIR | query | 1.13 MB | 1.13 MB | 1.13 MB | 1.13 MB | 26.01 MB |
|  | response | 128.1 KB | 128.1 KB | 128.1 KB | 128.1 KB | 19.52 MB |
|  | helper keys | 37.57 MB | 37.57 MB | 37.57 MB | 37.57 MB | 846.77 MB |
| Flat | query | 319.7 KB | 334.2 KB | 348.8 KB | 363.3 KB | 377.8 KB |
|  | response | 242.0 KB | 253.0 KB | 264.0 KB | 275.0 KB | 286.0 KB |
|  | helper keys | 1.42 MB | 1.42 MB | 1.42 MB | 1.42 MB | 1.42 MB |
| Respire | query | 75.0 KB | 117.9 KB | 120.6 KB | 206.3 KB | — |
|  | response | 8.0 KB | 8.0 KB | 8.0 KB | 8.0 KB | — |

Query bytes: Ours/Standard/Layerwise/Flat/Cuckoo are seed-compressed protocol models (one 14.5 KB packed BFV query per PIR call); SPIRIT+RC and TreePIR are actual serialized bytes; Respire is its parameter model. Response bytes are actual serialized bytes everywhere. Helper keys are uploaded once per session; SPIRIT's adapter has no key serializer and Respire reports none.

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
