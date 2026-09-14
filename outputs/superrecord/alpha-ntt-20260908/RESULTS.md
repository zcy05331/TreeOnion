# Generic alpha NTT optimization results

Implemented in the shared Generic composite-m32 preparation path; timing here is for h=3. Local x86_64 Rosetta, 256 MiB full tree, 32B entry, G=64, P=1, N0=352, b=7, expansion height 9. Both orders use 100 measured queries plus three warmups per implementation; 400 measured queries passed the existing exact returned-entry checks.

| Metric | Original | NTT-domain pyramid |
|---|---:|---:|
| Transformed ciphertexts per query | 705 | 352 |
| Integrated alpha preparation (ms) | 17.428 | 11.456 |
| Total server time (ms) | 566.123 | 556.137 |
| Query payload, modeled (B) | 14880 | 14880 |
| Response payload, actual (B) | 11264 | 11264 |

Integrated preparation time fell 34.27%; total mean server time fell 1.76% (throughput +1.80%). Other stages also fluctuate: the direct preparation saving is about 5.97 ms, while total observed saving is 9.99 ms. The total difference should not all be attributed to the removed NTTs. These are local results, not a new Clemson/Wisconsin full-scale measurement.

| Order | Original total ms | Optimized total ms |
|---|---:|---:|
| old then new | 566.007 | 558.010 |
| new then old | 566.238 | 554.264 |

## Stage means

| Stage | Original ms | Optimized ms |
|---|---:|---:|
| expand | 75.055 | 71.342 |
| complete | 36.979 | 36.758 |
| prepare | 17.428 | 11.456 |
| scan | 347.819 | 348.819 |
| finalize | 11.559 | 11.157 |
| fold | 44.095 | 44.137 |
| rotate | 17.469 | 17.486 |
| project | 14.163 | 14.040 |
| pack | 0.082 | 0.082 |
| switch | 0.372 | 0.372 |

## Validation and provenance

- 40/40 regression tests plus CLI checks passed; four h3 result-validation tests passed.
- Every NTT coefficient and m32 lo/hi buffer matches the old builder for widths 1, 2, 3, 5, 64, 351, 352, 513 with random and modulus-boundary inputs; odd carries and source immutability are checked.
- Independent alternating same-input preparation microbenchmark (100 pairs, including m32 split): 17.480 ms to 10.412 ms, a 40.43% reduction. The integrated-query preparation measurement above has different allocation/cache context.
- Implementation commit: 8d592da. Pinned benchmark/harness commit: e425c6f.
- Control: original 5a05443 code plus only the same benchmark pin; patch retained at local/benchmark-pin.patch. No optimized pyramid or updated transform-count model is backported into the control.
- The first automatic-selection attempt under local/full-query-ab chose N0=176 and hit the general h1/h3 validator. It is retained as an excluded exploratory run, not part of these totals.
- Existing decryption-only/noise-diagnostic policy is unchanged. No protocol, key, encoding or ciphertext-modulus changes.
