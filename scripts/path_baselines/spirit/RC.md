# SPIRIT response compression adapter

This adapter implements the response-rotation and MultiResMerge construction from SPIRIT §5.1–5.2 over the public SPIRIT batch protocol. The public checkout remains unchanged; `patch_spirit_rc.py` adds the required public database layout to the staged build copy.

## Public layout

Let `n` be the RLWE polynomial degree, `d = 32` the bytes per record, and `rho = n / d`. An RC bucket has `n` rows, `rho` records per column, and `ceil(bucket_item_count / rho)` columns. Record slot `s` is stored in column `floor(s / rho)`; its byte `j` is stored at row `(s mod rho) + j * rho`. Column selection still uses upstream SPIRIT's private LWE query. The resulting bucket response is one RLWE ciphertext.

For `n = 2048`, `rho = 64`; a batch of 32 with 48 PBC buckets fits in one final ciphertext. Height 24 contains `2^25 - 1` records, or `1 GiB - 32 bytes`, and each path contains 25 records including the root.

Standalone SPIRIT is not a retained baseline in this tree. The adapter keeps only the RC public layout and rejects `--scheme spirit` instead of silently mapping it to response compression.

## Encrypted rotation and merge

The client packs six selected-residue bits per bucket at `n = 2048` into one RNS-RGSW query ciphertext. The server expands this public upload into the encrypted per-bit selectors. The server conditionally multiplies the bucket ciphertext by `X^(-2^k)` using an external product for each bit. The desired record then occupies coefficients `j * rho`. Neither its residue nor a selected response chunk is supplied in plaintext.

MultiResMerge first scales ciphertext components by `rho^-1` in each coefficient modulus. For descending levels `v = log2(rho)-1, ..., 0`, set `w = 2^v` and `eta = n/w + 1`, then merge each adjacent pair using

```
C = A + X^w B + tau_eta(A - X^w B).
```

A missing right input is zero. Unary nodes continue through every remaining level, projecting out all unwanted residues. Each pair or unary node uses one Galois key switch. The output record for bucket `i` occupies coefficients `bit_reverse(i, log2(rho)) + j * rho`. The client decrypts only the returned compressed ciphertext.

With 48 buckets and `rho = 64`, rotation uses 288 external products and merge uses `24 + 12 + 6 + 3 + 2 + 1 = 48` key switches. The serialized merge Galois keys, selector expansion Galois and relinearization keys, packed offset ciphertext, and expansion parameters are included in query communication; generating them is included in client query time. Selector expansion has a separate server timing field.

## Packed selector query

The response context uses coefficient primes `72057594037641217` and `1152921504606830593`. Selector expansion keeps those as its data primes and adds the special prime `1152921504606683137`, yielding coefficient-modulus bit counts `{56, 60, 60}` at the same degree 2048. The client extends the same ternary secret into the added prime and generates public expansion Galois and relinearization keys.

`spirit_rc_selector_pack.cpp` uses upstream `hint_query_encrypt_rns` on the client. Its server function calls upstream's actual public offline/online expansion operations and secret switching directly. It accepts no secret key, decryptor, or plaintext offsets. Although upstream `unpack_hint_query_rns` has `Decryptor` and `shift_values` arguments, `shift_values` is unused and its nested `unpack_rlwe_offline_online` explicitly ignores the decryptor.

For 48 buckets and six bits per bucket, the measured selector upload is 65,713 bytes of ciphertext, 2,378,512 bytes of expansion keys, and 129 bytes of serialized expansion parameters. Merge keys and the underlying SPIRIT query are additional reported terms. The unpacked selectors exist only as a test control and a server-side intermediate.

## Verification

`spirit_rc_test` encrypts dense polynomials with nonzero values at every coefficient, chooses multiple private residues and public columns, and verifies the full 32-byte record for bucket counts 1, 2, 3, 48, and 64. Odd counts and a single input exercise unary projection stages. A shift-and-add negative control must fail on the same dense data. The packed-selector test verifies all 288 expanded bits by applying each selector to a dense 2048-coefficient encrypted polynomial. Full-path tests additionally exercise real database setup, PBC placement, LWE query, answer conversion, rotation, merge, and extraction against the deterministic independent record oracle, and compare the default packed query with independently encrypted per-bit selectors on the same response. H8 leaves 0, 17, 23, and 255 verify nine path records and seven padded records each.

## Limitations

The RC public layout requires `rho` to be a power of two and the PBC bucket count to be at most `rho`. It deliberately fixes one response polynomial per bucket. Unsupported parameter shapes are rejected before generating selectors. The public artifact parameter set at degree 2048 uses coefficient-modulus bits `{56, 60}` and plaintext modulus 65537; these exceed SEAL's standard degree-2048 `tc128` modulus limit, so the response and expansion contexts use `sec_level_type::none`; this is not a verified 128-bit security parameter set. The artifact's security setting and the expansion primes must be reported explicitly with experiment results.
