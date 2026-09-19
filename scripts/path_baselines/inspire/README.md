# InsPIRe full-path baseline adapter

This adapter runs a Merkle-path baseline by issuing one real Poulpy InsPIRe
query per tree level, including the root. It uses the public third-party
`poulpy-fhe/poulpy-pir` implementation staged under
`.benchmarks/inspire-path/poulpy-pir`; no author-maintained InsPIRe code was
found in the visible author publication page/GitHub lookup during integration.

The adapter is intentionally isolated from the project C++ core and existing
CLI/schema surfaces.

This checkout currently has only stable Rust installed. Poulpy 0.7 enables the
unstable `trait_alias` feature in `poulpy-hal`, so local verification uses
`RUSTC_BOOTSTRAP=1`. Poulpy itself pins nightly `nightly-2026-05-14`; on a
server with `rustup`, use that toolchain instead. Keep the Cargo target under
`.benchmarks/inspire-path` so build products stay in the isolated staging area.

Fresh staging recipe:

```bash
mkdir -p .benchmarks/inspire-path
git clone https://github.com/poulpy-fhe/poulpy-pir .benchmarks/inspire-path/poulpy-pir
git -C .benchmarks/inspire-path/poulpy-pir checkout 533081a74301c8ba6ddd5e1dfc0c9daa6e3e75ef
```

Example:

```bash
scripts/path_baselines/inspire/inspire_path_adapter.py \
  --height 8 \
  --leaf-file /tmp/inspire-leaves.txt \
  --warmup 1 \
  --trials 2 \
  --output /tmp/inspire-path.json
```

The wrapper runs:

```bash
RUSTC_BOOTSTRAP=1 CARGO_TARGET_DIR=.benchmarks/inspire-path/target \
cargo run --release --manifest-path scripts/path_baselines/inspire/Cargo.toml -- ...
```

On an AVX-512F host, select Poulpy's optimized backend through the wrapper:

```bash
INSPIRE_PIR_FEATURES=avx512-fhe \
RUSTFLAGS="-C target-feature=+avx512f" \
scripts/path_baselines/inspire/inspire_path_adapter.py ...
```

For AVX2/FMA hosts, use `INSPIRE_PIR_FEATURES=avx2-fhe` with
`RUSTFLAGS="-C target-feature=+avx2,+fma"`. The emitted JSON records the backend
actually compiled into the binary.

`--leaf-file` accepts ASCII decimal leaf ids, one per line. The first
`warmup + trials` ids are consumed. Every recovered 32-byte node is checked
against the deterministic tree-node generator:

```text
state = 0x747265654e6f6465
      ^ level * 0xd6e8feb86659fd93
      ^ node_index * 0xa5a3564e27f8862f
payload[word] = splitmix64(state + word), word = 0..3, updating state each word
```

This matches `scripts/path_baselines/workload.py` and
`scripts/path_baselines/common/path_workload.h`.

The JSON report separates:

- actual measured crypto timings and serialized query/response bytes from the
  Poulpy run, with schema `path-baseline-v1` and scheme `inspire_layerwise`;
- layout/model fields such as logical tree nodes and unused padded payload
  capacity;
- implementation provenance, which is `third_party_poulpy_pir`, not official
  author code.

Local Apple-Silicon scalar smoke, height 8 with one warmup and two measured
samples, passed the common validator with `poulpy_cpu_ref::FFT64Ref`. Observed
elapsed time was 19.10 s and peak RSS was 3,057,844,224 bytes using
`scripts/read_process_peak_memory.py`; measured mean server time was 174.97
ms/path, query bytes 1,926,297/path, response bytes 295,317/path. Treat this as
a correctness/integration gate, not the fastest candidate for formal reporting.
