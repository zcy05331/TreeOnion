# OnionTreePIR

Implementation of **OnionTreePIR: Efficient Private Retrieval of Tree Paths**.

OnionTreePIR is a single-server PIR scheme that lets a client privately
retrieve all nodes on a root-to-leaf path of a public binary tree, such as a
Merkle proof. The client sends one query that locates only the leaf; the server
derives the location of every path node from it, selects one node per level,
and merges all of them into a single response. Retrieving a whole path costs
about as much as retrieving a single entry with the underlying scheme,
[OnionPIRv2](https://eprint.iacr.org/2025/1142).

## How it works

1. **Encode.** Each tree level is stored as its own database of plaintext
   polynomials. A 32-byte node is split into `P=3` pieces of `g=8`
   coefficients each.
2. **Query.** The client splits the leaf index into `(gamma | alpha | beta)`,
   sends `alpha` one-hot and the other bits packed in one BFV ciphertext.
3. **Derive.** The server expands the query and, since every path node's index
   is a prefix of the leaf index, drops low bits per level to obtain the
   encrypted location of every path node.
4. **Select, rotate, merge.** For each level the server selects the polynomial
   holding the path node, rotates the node to coefficient 0, then merges all
   levels into one ciphertext, modulus-switches it and returns it.
5. **Decode.** The client decrypts and reads the node of level `l` at the
   bit-reversed coefficient positions.

For Merkle proofs, the tree is preprocessed by swapping every node with its
sibling, so the retrieved path is exactly the proof.

## Parameters (paper configuration)

| Parameter | Value |
|---|---|
| Ring degree `n` | 2048 |
| Ciphertext modulus `q` | 536752129 · 536813569 ≈ 2^58 |
| Plaintext modulus `t` | 4099 (12 payload bits per coefficient) |
| Response modulus `q'` | 4169729 ≈ 2^22 |
| 32-byte node layout | `g=8`, `P=3`, `rho=256` |
| Estimated security | 116 bits |

## Results

Trees with 2^22 to 2^27 leaves of 32-byte nodes (256 MiB to 8 GiB), single
thread on an Intel Xeon Platinum 8358:

- Online communication: **25.5 KiB** (14.5 KiB query + 11.0 KiB response),
  the same as one OnionPIRv2 single-entry query.
- Server time: **0.35 s to 5.74 s**, 10–20% above a single OnionPIRv2 query.
- Up to 30× faster server computation and 35× less communication than the
  level-wise, path-per-leaf, batch PIR (OnionPIRv2 + PBC, Respire) and
  TreePIR baselines.

At 1 GiB (2^24 leaves):

| Scheme | Communication | Throughput |
|---|---:|---:|
| OnionTreePIR | 25.5 KiB | 1223 MB/s |
| Level-wise | 463.5 KiB | 894 MB/s |
| Path-per-leaf | 25.5 KiB | 137 MB/s |
| OnionPIRv2 + PBC | 944.7 KiB | 240 MB/s |
| Respire-B | 128.6 KiB | 37 MB/s |
| TreePIR + OnionPIRv2 | 612.8 KiB | 573 MB/s |

## Build and test

Requires a C++20 compiler and Intel HEXL 1.2.6.

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Benchmark \
  -DACTIVE_CONFIG=CONFIG_N2048_K1_COMP -DPIR_EP_ROWS=3 -DPIR_EP_LOW_BITS=32 \
  -DHEXL_DIR=/path/to/hexl/lib64/cmake/hexl-1.2.6
cmake --build build -j8
./build/Onion-PIR --test all
```

- `--test all` runs the full correctness suite.
- `--test tree_multiplane` runs the scheme end to end on a small tree.
- `--test merkle_sha256_e2e` retrieves SHA-256 Merkle proofs through the
  serialized query and response and verifies them against the root.

The `-DPIR_EP_ROWS=3 -DPIR_EP_LOW_BITS=32` flags select the paper's
configuration; omit them for the default exact gadget. A 128-bit security
variant is `-DACTIVE_CONFIG=CONFIG_N2048_K1_COMP_S128 -DPIR_EP_ROWS=3
-DPIR_EP_LOW_BITS=26`.

In the code, the paper's node *pieces* are called *planes*. See the
[source map](src/README.md) for where each part lives.
