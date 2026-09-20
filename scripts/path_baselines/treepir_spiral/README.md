# TreePIR+Spiral Full-Path Adapter

This adapter serves one root-included ancestor path by running the upstream
TreePIR Spiral `Seperated` binary once per non-root CSA color bucket plus once
for a one-record root bucket. It writes the shared `path-baseline-v1` JSON with
`scheme: treepir_spiral`.

The non-root buckets use the C++ SubCSA indexing port from the existing TreePIR
benchmark patch. The bucket payloads are the shared 32-byte
`synthetic_tree_node_bytes(level, local_index)` records, placed at the
color-local offsets selected by SubCSA for the requested warmup/measured leaves.
The root is not returned from the oracle: it is staged as its own one-record
Spiral database and queried at local index 0.

## Build the staged Spiral CPU binary

The adapter owns a reproducible staged build under `.benchmarks/spiral-path/`.
It copies the read-only upstream `TreePIR/Spiral/Seperated` source, applies only
local adapter patches, and builds with the repository HEXL x86 compiler wrapper
and local Homebrew/Anaconda headers/packages:

```bash
python3 scripts/path_baselines/treepir_spiral/build_spiral_cpu.py --height 8 --jobs 2
python3 scripts/path_baselines/treepir_spiral/build_spiral_cpu.py --height 10 --jobs 2
python3 scripts/path_baselines/treepir_spiral/build_spiral_cpu.py --height 24 --jobs 2
python3 scripts/path_baselines/treepir_spiral/build_spiral_cpu.py --height 26 --jobs 2
```

The staged source patches are intentionally narrow:

- `TREEPIR_SPIRAL_ROOT` overrides upstream `/tmp/Spiral`; the default remains
  `/tmp/Spiral` if the env var is unset.
- `sched_getcpu()` is replaced with `0` for macOS.
- the simdjson loader is replaced by the already-linked nlohmann JSON loader so
  the x86 build does not link the arm64 Homebrew simdjson library.
- the printed positioned-check result uses upstream's computed
  `hashIsPositionedCorrectly`, not the weaker `hashExistsInFile` value.
- the decode bound assertion checks the plaintext coefficient capacity; upstream
  compared a coefficient offset against a hash-record count and aborted valid
  p=256 indices such as 32.

## Run shape

```bash
scripts/path_baselines/treepir_spiral/treepir_spiral_path_adapter.py \
  --height 24 \
  --leaf-file leaves.txt \
  --warmup 3 \
  --trials 16 \
  --output treepir_spiral.json \
  --spiral-bin .benchmarks/spiral-path/h24/build/Seperated/Seperated \
  --work-root /tmp/treepir-spiral-path-adapter
```

Each bucket process gets its own private `TREEPIR_SPIRAL_ROOT`; the runner does
not move, symlink, or overwrite a shared `/tmp/Spiral`. Pass `--keep-work-dirs`
to preserve staged DB/query inputs and upstream metric files after a failed or
formal run.

The adapter builds each CSA color bucket once from all canonical non-root tree
nodes, independent of the selected leaf schedule. It follows the public SubCSA
color split recursively, appending `node_bytes(level, local)` to each color's
local slot in exactly the same order used by the query-index mapper. There are
no secret-query fillers inside actual color buckets. Spiral's own public padding
beyond the JSON database capacity remains internal to the upstream plaintext
packing.

Per-sample timings come from that sample's own upstream metric entry for each
bucket. Missing query/server/decode metrics are errors. Query/response bytes are
labeled as the upstream Spiral communication model metrics, not mixed with
actual serialized-byte measurements.

## Local smoke status

On this Apple-Silicon host, H8 staged CPU binaries build successfully with the
local x86 HEXL route and complete a real encrypted shared-DB smoke run with one
warmup and two measured leaves using exactly H+1 upstream processes. H8/H10 use
the smallest valid public shape `num_expansions=1, further_dims=1`; H24 color
buckets require `6,7` and H26 color buckets require `8,7` to cover their bucket
capacity at 256 hashes per Spiral plaintext record, while the root bucket
remains `1,1`.
