# TreePIR + SealPIRplus core adapter

This is a bounded in-process backend pilot harness. It consumes the JSON produced by the VBPIR TreePIR runner in `--validate-index-only` mode, then runs one SealPIRplus core server/client per TreePIR color bucket. It bypasses the SealPIRplus gRPC wrappers and links only the upstream core files: `pir.cpp`, `pir_client.cpp`, and `pir_server.cpp`.

Linux build recipe:

```bash
cmake -S scripts/treepir_sealpir -B scripts/treepir_sealpir/build \
  -DSEAL_DIR=/path/to/seal/lib/cmake/SEAL-4.0
cmake --build scripts/treepir_sealpir/build -j
```

Local bundled SEAL example, when available:

```bash
cmake -S scripts/treepir_sealpir -B scripts/treepir_sealpir/build \
  -DSEAL_DIR="$PWD/.benchmarks/seal-install-x86_64/lib/cmake/SEAL-4.0"
cmake --build scripts/treepir_sealpir/build -j
```

Example run:

```bash
scripts/treepir_sealpir/build/treepir_sealpir_adapter \
  --input /path/to/vbpir-validate-index-only.json \
  --output /tmp/treepir-sealpir.json \
  --warmup 3 --trials 16 --pir-d 2
```

For schema-only H24/H26/H27 checks without allocating color databases:

```bash
scripts/treepir_sealpir/build/treepir_sealpir_adapter --input index.json --validate-input-only
```


At configure time, CMake copies the SealPIRplus core sources into the build directory and applies `patch_sealpir_core.py` there. The upstream `.benchmarks/TreePIR/SealPIRplus` checkout is left untouched. The adapter reports per-path and per-color server stage timings for encrypted query expansion, query NTT, database/intermediate plaintext NTT, multiply-accumulate, inverse NTT, recursive decomposition/mod-switch, and the final response mod-switch required before transport serialization.

The default BFV parameters are `N=8192`, `plain_logt=20`, `d=2`, symmetric queries, batching, and recursive mod switching. Full H24/H26/H27 runs process colors sequentially to keep only one color database resident at a time; per-path server time is therefore a sum across color-server stages, including the required SealPIR response mod-switch stage, not a single all-colors-hot-cache server wall time.
