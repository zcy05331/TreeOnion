# Onion Merkle PIR

Research prototype for privately retrieving a tree path. The primary algorithm
uses h=1 and the original `(gamma, alpha, beta)` index layout with multiple
planes. See the [source map](src/README.md) and [script guide](scripts/README.md)
for implementation boundaries.

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

## Baselines

The supported comparison contains exactly these seven baseline families:

| Baseline | Implementation |
|---|---|
| StandardOnionPIR | Original single-block OnionPIRv2 |
| SPIRIT+RC | SPIRIT with packed selectors and response compression |
| Layerwise tuned | One PIR per tree level, using measured layout tuning |
| Cuckoo batch tuned | Cuckoo batch placement with measured layout tuning |
| TreePIR | Colored-path baseline with VBPIR/SealPIR backend tuning |
| Flat | One whole-tree PIR per requested path level |
| Respire | Canonical tree-path workload on Respire batch PIR |

StandardOnionPIR retrieves one plaintext block; it is a speed reference rather
than a full-path protocol. The primary, SPIRIT+RC and Respire workloads include
the root; the legacy Merkle and colored-path baselines omit it. Reports retain
these useful-node counts and distinguish measured bytes from modeled bytes.

## Build and test

Use C++20 and Intel HEXL 1.2.6. Supply its CMake package path explicitly:

```bash
cmake -S . -B build -DCMAKE_BUILD_TYPE=Benchmark \
  -DACTIVE_CONFIG=CONFIG_N2048_K1_COMP -DHEXL_DIR=/path/to/hexl/lib64/cmake/hexl-1.2.6
cmake --build build -j8
./build/Onion-PIR --test all --experiments 3 --warmup 1
```

The default build retains exact EP6 arithmetic for the core baselines. The
primary performance configuration uses `-DPIR_EP_ROWS=3 -DPIR_EP_LOW_BITS=32`
with exact KS8; validate its encrypted primary tests before measuring it.
SPIRIT+RC requires its pinned SEAL fork, and Respire requires its pinned Rust
source. Their separate build/smoke gates are described in their adapter READMEs.
After preparing these dependencies, `scripts/run_regression.sh` runs the core
and integrated script gates, including an encrypted Respire smoke. Full Python
discovery additionally requires `SPIRIT_PATH_ADAPTER` to name the built RC
adapter and `RESPIRE_PATH_ADAPTER_BIN` to name the built Respire adapter.

For a single 1 GiB-tier primary run with 32-byte entries:

```bash
TREE_MULTIPLANE_N0=512 TREE_MULTIPLANE_PACKING=mlwe \
  ./build/Onion-PIR --test tree_multiplane_bench --leaf-count 16777216 \
  --experiments 50 --warmup 3 --trial-seed 7 --benchmark-json outputs/primary.json
```

For a layout-fixed N0 sweep:

```bash
python3 scripts/benchmark_tree_multiplane.py --binary build/Onion-PIR \
  --output-dir outputs/primary-n0 --leaf-count 16777216 --entry-bits 256 \
  --n0-values 128,256,512,1024 --experiments 8 --warmup 3 --trial-seed 7
```

## Experiment policy

The current matrix uses 32-byte entries at 256 MiB, 512 MiB, 1 GiB and 4 GiB,
corresponding to leaf heights 22, 23, 24 and 26. Normally use 50 measured queries;
Cuckoo batch, TreePIR and Flat use 8. Each point has 3 warmups and seed 7.
Record complete server latency, stage profiling, query/response bytes, setup,
peak memory, hardware and source revision. A fresh host requires fresh timings.
The serial runners wait for other computation to be idle and never signal it.
Resource-infeasible points are reported explicitly rather than counted as runs.

## Archived experiments

Earlier algorithms, campaign scripts and raw results remain in
[`codex/archive-full-pir-20260920`](https://github.com/zcy05331/OnionMerklePIR/tree/codex/archive-full-pir-20260920).
They are not active baseline implementations in main. Current artifacts belong
under `outputs/`; private drafts and `docs/` stay outside published source.
