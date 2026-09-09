# Initial local small-gadget result (v1)

32 B, 2^22 leaves (about 256 MiB), fixed h3/G64/P1/N0=352/b7, expansion height 9. x86_64 Rosetta. Each configuration has two execution orders, 50 measured queries and 3 warmups per order. All 500 measured queries passed requested-plaintext checks.

| Configuration | Total ms | Rotation + projection ms | Samples |
|---|---:|---:|---:|
| baseline | 546.235 | 31.087 | 100 |
| kernel | 536.037 | 29.251 | 100 |
| ep3-s24 | 488.692 | 22.031 | 100 |
| ep3-s32 | 482.219 | 21.852 | 100 |
| ep2-s36 | 466.181 | 19.378 | 100 |

Baseline source a9503fb; implementation 5d5f6c3. Kernel changes retain EP6/KEY10/KS8. EP3-s24 uses base12/drop24; EP3-s32 uses base9/drop32; EP2-s36 uses base12/drop36. KEY10 and KS8 remain exact in the successful parameter points.

Each query is 14880 modeled seeded bytes; actual response is 11264 bytes. Helper-key upload models are unchanged for these EP-only points.

Build-concurrent pilots are excluded from this table. Rejected earlier points and their logs remain under pilots/. These timings do not establish native AVX or 1 GiB performance.
