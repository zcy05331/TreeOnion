# OnionTreePIR

Single-server PIR that retrieves a whole root-to-leaf path of a binary tree,
such as a Merkle proof, with one query and one response. It builds on
OnionPIRv2 and keeps its ring, moduli, error sampler and first-dimension
kernel. The [source map](src/README.md) lists where each part lives.

## Primary algorithm

For an E-bit entry and c payload bits per coefficient, a plane reserves g
coefficients and `P=ceil(E/(c*g))`. Choose a dyadic divisor g of the ring degree
n with `rho=n/g>=2` and **P<=6**. First maximize `E/(c*g*P)`; among layouts with
the same utilization, choose the smallest P. Only then tune N0 and the derived
fold width b by complete server time.

At `n=2048, t=4099, c=12`:

| Entry bytes | g | P |
|---:|---:|---:|
| 16 | 4 | 3 |
| 32 | 8 | 3 |
| 48 | 32 | 1 |
| 64 | 16 | 3 |
| 128 | 32 | 3 |

At tree level ell, `R=2^max(ell-log2(rho),0)`, node `p+u*R` occupies
polynomial p at coefficients `u+digit*rho`. All planes share the same query,
expanded alpha pyramid, and beta/gamma selectors. Selection and rotation run
per plane, then MLWE combines the fragments into responses. Projection remains
a response-packing diagnostic of this same algorithm.

The leaf index is `i=gamma*(N0*2^b)+alpha*2^b+beta`. N0 is a power of two.
The alpha pyramid is built in the NTT domain; interleaved and split-column
query buffers support a matched implementation comparison. Correctness checks
cover the complete response plaintext, including padding; noise margins are
diagnostic and do not impose a one-bit admission threshold.

## Build and test

Use C++20 and Intel HEXL 1.2.6. Supply its CMake package path explicitly:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Benchmark \
  -DACTIVE_CONFIG=CONFIG_N2048_K1_COMP -DHEXL_DIR=/path/to/hexl/lib64/cmake/hexl-1.2.6
cmake --build build -j8
./build/Onion-PIR --test all --experiments 3 --warmup 1
```

`--test all` runs the correctness suite. `--test tree_multiplane` runs the
scheme end to end on a small tree, and `--test merkle_sha256_e2e` retrieves
SHA-256 Merkle proofs through the serialized query and response and
recomputes the trusted root.

The default build uses the exact EP6 selector gadget. The paper's
configuration adds `-DPIR_EP_ROWS=3 -DPIR_EP_LOW_BITS=32`, a three-row
approximate gadget with exact KS8. The 128-bit configuration is
`-DACTIVE_CONFIG=CONFIG_N2048_K1_COMP_S128 -DPIR_EP_ROWS=3 -DPIR_EP_LOW_BITS=26`.
