# VIA + PBC baseline

VIA (Liu, Wang and Zhang, *VIA: Communication-Efficient Single-Server Private
Information Retrieval*, ePrint 2025/2074, S&P 2026, Section 3 and Figure 4) has
no native batch mode, so it runs behind the OnionPIR cuckoo baseline's PBC
instance, like Spiral + PBC and KsPIR + PBC: same records, hashes, bucket
positions and seed-7 leaves (`../kspir_pbc/pbc.hpp`), one VIA query per bucket,
dummy buckets asking for position 0. A path query samples one key pair
(S1, S2), so its bucket queries share one ring-switching key. VIA has no offline
communication; the query carries all key material.

## Why the scheme is written from the paper

The authors' repository (`owniai/VIA@f65aa9d`) has a VIA server, but its first
dimension multiplies `database[0]` by the first DMux output for every row and
column, databases above 2 GB only simulate their memory traffic, the database is
random and no answer is checked. `via_base.hpp` implements Figure 4 directly
and reuses the ring arithmetic, gadget decomposition, encryption and IFMA dot
products of the VIA-B adapter (`../via_b/via.hpp`).

## What runs

Per bucket, Figure 4 with the Section 3.2 optimizations that the paper's
evaluation and the authors' code use:

1. the client sends the third DMux level: eight RLWE ciphertexts over
   (n1, q1), the one at alpha's top three bits encrypting floor(q1/p) X^-t and
   the others zero; the server runs the remaining log I - 3 levels with the RGSW
   control bits;
2. modulus switch q1 -> q2 and first dimension over the bucket's I x J matrix;
3. ring switching of every column to R_{n2, q2} under S2;
4. CMux over the low log J - 3 bits of beta with RGSW select bits over
   (n2, q2); the last three levels are left out and their eight inputs are the
   answers;
5. blinded extraction: each answer's mask is switched to q3 and only the
   record's 32 coefficients of B to q4.

With p = 256 a 32-byte record is 32 coefficients and a polynomial holds 64
records. Record gamma's byte u sits at coefficient t + 4u with
t = 128 * floor(gamma / 4) + gamma mod 4: X^-t moves it to coefficient 4u,
which ring switching maps to coefficient u of R_{n2} (the paper's
iota_{n2 -> n1} layout with 16 records per R_{n2} polynomial). Bucket position
pos is (alpha, beta, gamma) with pos = gamma * I * J + alpha * J + beta.

The client decodes the answer that holds beta's column and compares every real
record with the Merkle oracle outside the timers; the JSON also reports the
largest decoding error in q4 units (half a plaintext step is 64).

## Parameters

| | value |
| --- | --- |
| rings | n1 = 2048, n2 = 512 |
| moduli | q1 = 268369921 * 536608769 (57 bits), q2 = 34359214081 (35), q3 = 2147352577 (31), q4 = 2^15 |
| plaintext | p = 256 |
| keys and errors | S1 uniform on [-2, 2], E1 Gaussian sigma 1; S2 ternary, E2 Gaussian sigma 4960 |
| gadgets (length, log base) | DMux (2, 19) approximate over q1; CMux (4, 5) for A and (3, 5) for B, approximate over q2; ring switching (4, 5) approximate over q2 |

Moduli, degrees, p, error widths and gadget lengths are the paper's (Section
5.1, Appendix B, Table 5); the gadget bases are the powers of two of the authors'
code (the paper lists 370758 and 24). The runner pilots log_rows (the default
`0.65 * sqrt(polys)` balances a DMux node and its row's switch to q2, about 2.5
times the cost of one column's ring switch and CMux node, against the columns)
and its two neighbours, and the GCC and icpx builds, and keeps the fastest.

## Sizes

Query bytes are reported seeded (every uniform mask stands for one 32-byte seed
per path, as the paper counts) and uncompressed. Per bucket the query holds the
8 sent DMux ciphertexts over (n1, q1), log I - 3 control RGSW ciphertexts of 4
rows over (n1, q1) and log J - 3 select RGSW ciphertexts of 7 rows over
(n2, q2); per path it adds the ring-switching key, 16 rows over (n2, q2). The
response is 8 answers per bucket of 512 q3 and 32 q4 coefficients (2044 bytes).
There are no public parameters.

## Build and run

    cmake -S . -B build -DHEXL_DIR=<hexl-1.2.6 cmake dir>
    cmake --build build
    build/via_pbc --self-test --fixture ../kspir_pbc/tests/fixtures/cpp_h22_bucket_member_counts.json
    build/via_pbc --height 24 --output via_pbc.json

The self-test checks the H=22 bucket member counts and the seed-7 leaf
schedules against the C++ benchmark, then decodes records of two toy buckets:
one with server DMux and CMux levels, one without either and with answers past
J. `scripts/run_via_matrix.py --scheme via_pbc` pins one CPU and runs the pilots
and the formal rows. The IFMA kernels need AVX512-IFMA (`-march=native` on the
benchmark nodes); other hosts use the scalar fallbacks.

## Limitations

- S2 is ternary, not the paper's Gaussian with sigma 4960 (the authors' code
  draws it uniformly from [-379, 379]). The CMux decomposes A approximately (the
  top 20 of 35 bits), and that rounding error is multiplied by S2: per CMux
  level its standard deviation is 16 times half a plaintext step with sigma
  4960 and 0.7 times with the code's key. As for VIA-B, the paper's error
  analysis leaves this term out. The 110-bit estimate was not rerun for the
  ternary key.
- The NTT-form database is stored as 35-bit words (32-bit plane plus a 4-bit
  plane): the first dimension reads 4.5 bytes per record byte instead of 8, and
  the three-fold replicated 8 GiB instance takes about 110 GiB.
- Keys, masks and noise come from seeded xoshiro256** streams, so a failing
  sample can be replayed; the timings do not depend on them.
