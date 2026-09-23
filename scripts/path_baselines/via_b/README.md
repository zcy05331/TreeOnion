# VIA-B baseline

VIA-B (Liu, Wang and Zhang, *VIA: Communication-Efficient Single-Server Private
Information Retrieval*, ePrint 2025/2074, S&P 2026, Section 4.7 and Figure 9)
on the root-excluded Merkle path workload with the seed-7 leaves (`pbc.hpp`,
shared with `../kspir_pbc/`).

- `--batch native` (default, scheme `via_b`) is the paper's VIA-B: the H
  sibling queries of a path are independent VIA-C queries over the whole
  database, and their answers are repacked. This matches the paper's
  communication figures (Table 2: 32 queries on 1 GB are 18.16 KB, 32 times the
  0.568 KB single VIA-C query).
- `--batch pbc` (scheme `via_b_pbc`) runs the same answer path once per bucket
  of the OnionPIR cuckoo baseline's PBC instance (same records, hashes, bucket
  positions; dummy buckets ask for position 0) and repacks the bucket answers.
  It is a separate variant, not the paper's protocol.

## Why the scheme is written from the paper

The authors' repository (`owniai/VIA@f65aa9d`) implements VIA and VIA-C only;
it has no homomorphic repacking, so no VIA-B. Its VIA-C does not return correct
answers and cannot be timed as is: the first dimension reads only
`database[0]`, databases above 2 GB only simulate their memory traffic
(repository README), the database is random and no answer is checked, and the
gadget decomposition treats the prime moduli as powers of two (for the 18-digit
base-32 conversion gadget it evaluates `1 << 89` in 64 bits). With its
plaintext modulus 256 the CMux approximation error also exceeds half a
plaintext step. `via.hpp` implements the paper's algorithms directly.

## What runs

Per query, the VIA-C answer path of Figure 8:

1. query decompression (Figure 6): each LWE ciphertext goes through the
   LWE-to-RLWE conversion of Section 4.1 (11 MLWE-to-MLWE levels, 2 key
   switches each) and the RLWE-to-RGSW conversion of Section 4.2;
2. DMux over I = 2^log_rows rows from `(0, floor(q1/p))`;
3. modulus switch q1 -> q2 and first dimension over the I x J matrix of the
   database (native) or bucket (pbc);
4. CMux over the J columns and CRot by the record's slot.

Per path, the answers are packed eight per ciphertext with the
MLWEs-to-RLWE homomorphic repacking of Section 4.6. As the paper's Remark
there prescribes, the intermediate MLWE keys are projections of S1, so
repacking uses the conversion keys (switched to q2 by the server) and adds no
public parameters. Each packed ciphertext is switched to q3, ring-switched to
R_{n2} and its B part rounded to q4 (Figure 7). A record is 32 bytes = 64
coefficients of 4 bits (n3 = 64), 32 records per polynomial; answer r of a
packed ciphertext occupies coefficients `8v + bitrev3(r)`.

Every recovered record is decoded and compared with the Merkle oracle outside
the timers; the JSON also reports the largest decoding error in q4 units
(half a plaintext step is 128).

## Parameters

| | value |
| --- | --- |
| rings | n1 = 2048, n2 = 512 |
| moduli | q1 = 137438822401 * 274810798081 (75 bits), q2 = 17175674881 (34), q3 = 8380417 (23), q4 = 2^12 |
| plaintext | p = 16 |
| keys and errors | S1 ternary; E1 Gaussian sigma 1024; S2, E2 Gaussian sigma 26 |
| gadgets (length, log base) | DMux (2, 16) approximate over q1; CMux and CRot (2, 7) approximate over q2; conversion and RLWE-to-RGSW (15, 5) exact over q1; ring switch (8, 3) exact over q3; repacking uses the conversion digits 9..14 after the q1 -> q2 switch |

Moduli, degrees, p, sigma values and gadget lengths are the paper's
(Section 5.1, Appendix B, Table 6), except where the next section says
otherwise. The runner pilots log_rows (the default `0.55 * sqrt(polys)` and its
two neighbours) and the GCC and icpx builds, and keeps the fastest.

## Build and run

    cmake -S . -B build -DHEXL_DIR=<hexl-1.2.6 cmake dir>
    cmake --build build
    build/via_b --self-test --fixture ../kspir_pbc/tests/fixtures/cpp_h22_bucket_member_counts.json
    build/via_b --height 24 --output via_b.json               # paper's VIA-B
    build/via_b --height 24 --batch pbc --output via_b_pbc.json

`scripts/run_via_b_matrix.py [--batch-mode pbc]` pins one CPU and runs the
pilots and the formal rows. The IFMA kernels need AVX512-IFMA (`-march=native` on the
benchmark nodes); other hosts use the scalar fallbacks.

## Limitations

- S1 is ternary, not the Gaussian with sigma 32 of Section 5.1. With sigma 32
  the CMux approximation error `n1 * Var(S1) * (q2 / B^l)^2 / 12` per selected
  level is larger than half a plaintext step at the paper's CMux gadget; the
  paper's Lemma C.1 omits Var(S1), and the authors' code samples a ternary S1.
  The paper's 110-bit estimate assumed sigma 32; the estimator was not rerun
  for the ternary key.
- Gadget bases are the powers of two of the authors' code (2^16, 2^7, 2^5,
  2^3), not Table 6's 55879, 81, 18 and 8. The conversion gadget uses 15 exact
  base-32 digits of the 75-bit q1 instead of 18 base-18 digits, which shortens
  the conversion keys by 3/18.
- The paper vectorizes multiple packed ciphertexts with Spiral's technique; here
  each group of eight answers is its own ring-switched ciphertext.
- The NTT-form database is stored as 34-bit words (32-bit plane plus a 2-bit
  plane): the first dimension reads 4.25 bytes per 4-bit coefficient instead of
  8, and the pbc variant's three-fold replicated 4 GiB instance fits in about
  105 GiB.
