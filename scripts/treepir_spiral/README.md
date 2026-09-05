# TreePIR Spiral CPU smoke path

This directory contains the minimal reproducible path for trying the upstream TreePIR Spiral `Seperated` CPU baseline against one real CSA color bucket at a time. It stages inputs and emits build/run commands; it does not modify `.benchmarks/TreePIR/Spiral`, does not submit remote jobs, and does not invent pilot timings.

## What is usable in this checkout

The viable entry point is `.benchmarks/TreePIR/Spiral/Seperated`. The checked-in separated binary path uses a hard-coded runtime root `/tmp/Spiral`, loads hash JSON from `/tmp/Spiral/Database/Colouring/Data/<argv3>`, reads every `*.query` file under `/tmp/Spiral/Query_Storage`, and writes evaluation files under `/tmp/Spiral/Evaluation/Data`. Its `--random-data` path throws in this fork, so a smoke run needs a real color-bucket JSON database.

The 32-byte H24/H26/H27 presets are in `.benchmarks/TreePIR/Spiral/Documents/Configuration (copy)`:

| height | build variables | upstream run shape |
| --- | --- | --- |
| H24 | `TEXP=8 TEXPRIGHT=56 TCONV=4 TGSW=10 QPBITS=20 PVALUE=256 QNUMFIRST=1 QNUMREST=0 OUTN=2` | `./spiral 9 7 46693 a` |
| H26 | `TEXP=8 TEXPRIGHT=56 TCONV=4 TGSW=11 QPBITS=21 PVALUE=256 QNUMFIRST=1 QNUMREST=0 OUTN=2` | `./spiral 9 9 175209 a` |
| H27 | `TEXP=16 TEXPRIGHT=56 TCONV=4 TGSW=11 QPBITS=20 PVALUE=256 QNUMFIRST=1 QNUMREST=0 OUTN=2` | `./spiral 9 10 354461 a` |

`PVALUE=256` packs two hex nibbles per plaintext coefficient. With `OUTN=2` and `poly_len=2048`, one Spiral DB polynomial can encode 256 32-byte hashes. The wrapper treats the input as one color bucket of 64-hex hash strings. The CSA adapter should supply local offsets within that bucket for the warmup and measured leaf paths assigned to the same color.

## Stage one color bucket

Provide a color-bucket JSON and an index-only JSON keyed by height. The common format is one list of local offsets for the selected color bucket; a list of lists is also accepted when you want to keep several query sets and select one with `--path-id`. This illustrative file uses small offsets only to show the shape:

```json
{
  "24": [0, 17, 42, 111],
  "26": [3, 81, 144, 201],
  "27": [[5, 92, 128], [8, 13, 233]]
}
```

Then stage H24, for example:

```bash
scripts/treepir_spiral/prepare_spiral_smoke.py \
  --height 24 \
  --database-json /path/to/color-bucket.json \
  --indices-json /path/to/color-local-offsets.json \
  --database-name 46693 \
  --manifest /tmp/spiral-h24-smoke-manifest.json
```

The wrapper validates that the database is a Spiral hash map and that the selected offsets are inside that bucket's loaded sequence length. It writes `/tmp/Spiral/Database/Colouring/Data/<database-name>` and one `.query` file containing the selected offsets. The generated manifest includes the build command and the exact binary invocation. Use one staged color file for the warmup/measured leaf paths of that same color; do not combine indices from different colors into one staged database.

## Build the CPU target

Install upstream dependencies with vcpkg, or use an environment where CMake can resolve `HEXL`, `nlohmann_json`, `simdjson`, and Boost 1.83. The helper accepts `--vcpkg-root` for the vcpkg toolchain path and `CXX` for the compiler if `clang++-12` is not on PATH.

```bash
CXX=clang++ scripts/treepir_spiral/build_spiral_cpu.sh \
  --height 24 \
  --spiral-root .benchmarks/TreePIR/Spiral \
  --build-dir /tmp/spiral-seperated-h24 \
  --vcpkg-root /path/to/vcpkg
```

The helper creates a small CMake overlay under the build directory. The overlay calls the same package discovery as the upstream top-level CMake, adds only the `Seperated` target, avoids the checked-in `clang++-12` compiler hardcode, uses Unix Makefiles because the upstream target expects make-time parameter variables, and preserves the upstream crypto code.

## Current local blockers

This checkout does not currently have a local Spiral build output, `clang++-12` is absent here, and `~/vcpkg` is absent here. A non-vcpkg system-package build may work if CMake resolves the same imported targets, but Boost 1.83 is the checked-in top-level requirement. The parameter-search caches `all_params*.pkl` in `Seperated/` are Git LFS pointer files, so `select_params.py` cannot be used locally until those are fetched or regenerated. The checked-in evaluator also limits `n_power` to below 25, which excludes H26/H27 without changes. For the bounded pilot, use the preset build variables above plus one real color bucket and its local query offsets rather than the generic evaluator.
