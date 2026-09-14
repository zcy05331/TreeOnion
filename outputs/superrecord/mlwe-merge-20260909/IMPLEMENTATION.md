# Generic MLWE response merge prototype

Implements the shared key-switch merge idea from SPIRIT Section 5.2 / Algorithms 4–5:
https://www.usenix.org/system/files/usenixsecurity26-zhang-zhou.pdf#page=12

The backend reuses full-degree RLWE buffers and existing BV Galois keys. At reverse
trace level v, set w=2^v and eta=n/w+1. For inputs A,B, compute
`A + X^w B + BVApplyGalois(A - X^w B, eta)` with one key switch.
Inputs are normalized by rho^-1 modulo each ciphertext limb before any switch.
Execute v=log2(rho)-1 through 0. A missing right child is zero, but its unary trace
still executes. The resulting input-i fragment occupies public residue BitRev(i),
so client extraction uses that public ordering separately in each response chunk.

The partial-group adaptation and explicit normalization are our implementation;
this does not import SPIRIT's stateful ciphertext conversion or change the PIR scan.
The caller must supply all log2(rho) trace keys. The current comparisons register
11 keys for both methods. Extra response input buffers are bounded by one chunk.

## Running

Set `SUPERRECORD_RESPONSE_PACKING=projection` (default) or `mlwe` on
`--test tree_superrecord_bench`. Database layout and existing planner calibration
remain fixed. The noise gate executes the chosen response method and binds its
result to that method. Experiments use `SUPERRECORD_MIN_NOISE_MARGIN=0`:
actual requested-path decryption decides acceptance; noise diagnostics remain.

The new `merge` stage includes shared trace operations, normalization and merging.
For response processing compare `rotate + project + pack + merge`, not just
`rotate + project`. The driver emits actual merge KS counts per sample.

## Validation

- RED: returning the first ciphertext unchanged failed the full-polynomial oracle
  at rho=2, after passing the identity case rho=1.
- Default EP6/KS8: 43/43 applicable regression tests passed.
- Approximate EP2/drop36 with exact KS8: primitive and complete-path tests passed.
- Primitive cases check every one of 2,048 plaintext coefficients, including dense
  unwanted coefficients, partial/odd groups, rho=256/2048 and unary trace levels.
- Full-path tests cover h1/h3, multi-chunk replies, all-lane decoding, method-bound
  noise gates and rejection of insufficient trace keys.
- Benchmark driver: 10 gadget/packing tests and 5 existing alpha A/B tests passed.

KS counts for 1 GiB / 32 B: h3, M=25,rho=32 -> 27; h1, M=75,rho=256 -> 79.
Performance measurements are stored separately from these operation counts.
