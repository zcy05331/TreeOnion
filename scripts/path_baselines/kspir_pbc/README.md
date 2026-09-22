# KsPIR + PBC cuckoo baseline

KsPIR (Luo, Liu and Wang, "Faster FHE-Based Single-Server Private Information
Retrieval", CCS 2024; `mmingluo/kspir@54c2f61`) has no native batch mode, so it
runs behind the same probabilistic batch code as the OnionPIR cuckoo and
Spiral+PBC baselines. `pbc.hpp` is a standalone copy of `src/cuckoo_batch.cpp`,
the Merkle helpers and `make_benchmark_trial_plan`; `--self-test` checks the
H=22 bucket member counts and the H=22/H=24 seed-7 leaf schedules against the
C++ benchmark's output. Rows differ from the OnionPIR cuckoo rows only in the
per-bucket PIR backend.

## Mapping

A KsPIR database with packing number r holds r x 2048 records of 8 KB (4096
16-bit values, plaintext modulus 65537). The adapter packs 256 Merkle records
per 8 KB item, so bucket position p is item x = p / 256 at byte (p % 256) * 32.
Item x is KsPIR record (column c = x mod 2048, packing t = x / 2048); its value
m sits in instance k = m mod r, row r * floor(m / r) + t, column c, which is the
layout `tests/test-pir.cpp` writes and recovers. Every column is inverse-encoded
before the upstream BSGS/NTT and CRT preprocessing. r is the smallest power of
two covering the largest bucket (`packingRLWEs` requires a power of two), and all
buckets share r and N1, so one set of BSGS and packing keys serves every bucket.

The online path is the upstream sequence: `matrix_vector_mul_bsgs_rns_crt_large`
(first dimension by BSGS key switching), r `externalProduct` calls and
`packingRLWEs`. Stage timers bracket those three calls. The client reduces
decrypted values mod p: `decrypt_bsgs_total` returns p for a plaintext 0 whose
noise is slightly negative (its negative branch adds p without reducing), which
the upstream test never exercises because it writes 1..4096.

## Parameters and sizes

`scripts/run_kspir_pbc_matrix.py` pilots the system GCC build against the
LLVM-based icpx build at 1 GiB (the authors recommend clang) and pilots the BSGS
split N1 in {64, 128, 256} per height, then measures 8 formal trials with the
winner. Query bytes are reported seeded (each uniform mask replaced by a 32-byte
seed, as the KsPIR paper counts: 152 KiB per bucket query) and uncompressed; the
response is the full packed ciphertext; the public-parameter bytes are the BSGS
and packing keys.

## Patch

`patches/kspir-build.patch` lets the build use HEXL 1.2.6 (the version every
other baseline links; same minor release as the recommended 1.2.5) and removes
the hardcoded `/usr/bin/clang++` so the compiler can be chosen.

Run on native x86 only: under Rosetta the upstream test itself decrypts to zeros.
