# Generic h1: packing-aware selection review

Reference archive: `outputs/superrecord/eval-20260910/census-run-root/outputs/matrix/1gb/generic.json`,
source2833130, EP3/drop32, exactKS8, MLWE,100 interleaved measured blocks.

| G/P/N0/b | Predicted ms | Measured ms | Merge KS | Public shifts |
|---|---:|---:|---:|---:|
| 8/3/512/7 | 1041.053 | 905.444 | 79 | 74 |
| 8/3/256/8 | 1167.709 | 1066.282 | 79 | 74 |
| 32/1/512/9 | 1184.428 | 1062.788 | 28 | 24 |

The model calibrates Projection and charges projection/placement even when
execution uses MLWE. Nevertheless its first h1 choice is the fastest measured
one. Fixing this accounting is justified, but does not establish a speedup.
There is a small second/third misranking; independent scan prediction errors
also contribute. Retuning is separate from the fixed-layout kernel experiment.

For M=P*(H+1),rho=n/G,r=log2(rho), actual merge KS is the sum over response
chunks g<=rho of sum(j=1..r,ceil(g/2^j)). Public shifts=M-ceil(M/rho).
Also account for M normalizations, one difference copy and post-Galois addition
per KS, and one additional addition/subtraction per binary merge. Unary trace
rounds remain required. The old projection coefficient cannot simply be reused
as a complete merge coefficient.

A bounded next h1 retune can retain the three measured controls and add
G4/P6/N0=512/b6. That new shape has no measured win: it is useful to test the
packing-aware ranking. N0=1024 requires expansion11 and crosses the current
AVX512 lazy-accumulation guard for the29-bit limbs; it must not be presented as
a free reduction in folding cost. G2/P11 saves scan bytes but incurs2145 private
rotations at H24, so it is not the first candidate to explore.

Previously tried and reverted: CRT compose using Barrett, and replacing candidate
zero-fill with reserve/push_back. Keep these failures in the search history.
