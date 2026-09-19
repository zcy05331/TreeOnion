# Additional full-path PIR baselines

These adapters extend refactored main `2261bbd` without changing the existing core,
commands or result schemas. Each protocol owns its build/staging, input handling,
real database preparation, encrypted execution and reporting. `common/` and
`workload.py` contain only the shared workload. The finite runner and report
validator consume the format below; they do not reinterpret protocol internals.

## Adapter ownership

| Modes | Implementation and build instructions |
|---|---|
| SPIRIT, SPIRIT+RC | [spirit/](spirit/README.md): upstream batch PIR and the encrypted response-compression prototype |
| VBPIR+PBC | [vbpir_pbc/](vbpir_pbc/README.md): PBC layout, native PIR execution, stage reporting |
| InsPIRe | [inspire/](inspire/README.md): third-party Poulpy implementation, one request per tree level |
| TreePIR+Spiral | [treepir_spiral/](treepir_spiral/README.md): public tree coloring, canonical bucket databases, Spiral execution |
| Respire | [respire/](respire/README.md): native batch path workload and exclusive compression stages |

Each directory separates workload/input, protocol execution and reporting.
The existing `src/` and its CLI are unchanged. External source trees and their
patched build copies remain outside the tracked implementation.

## Workload and CLI

Every adapter accepts:

```
<adapter> --height H --leaf-file leaves.txt --warmup W --trials N --output result.json
```

Additional protocol-specific flags are allowed. Leaves are zero-based ASCII
integers, one per line. Consume exactly the first W+N; warmups are a separate
prefix. One sample returns all H+1 ancestors, including root, in root-to-leaf
order; no sibling swapping. A complete binary tree has (1<<(H+1))-1 records.
BFS record index for level l and local index j is (1<<l)-1+j.

Data is exactly the existing `synthetic_tree_node_bytes` source in
`src/tree_database.cpp`. The shared C++ header exposes:

```
path_workload::node_bytes(level, local_index) // std::array<uint8_t,32>
path_workload::record_bytes(flat_index)      // std::array<uint8_t,32>
path_workload::path_indices(height, leaf)    // std::vector<size_t>
path_workload::read_leaves(filename)         // std::vector<size_t>
```

Golden record hex:

- (level0,index0): b8782335a8e76c4bf81ce235c978f777b654f924299739320bda3618dcc37790
- (level3,index6): 3849527f67cb7a2c02bb1678b692eb093d424f3e552d646e20c3159427402066
- (level24,index16777215): aa4765a84a38d7d577d573482b756942f01ad5caf1c03c7adfec59dcda06b3cd

If a protocol pads the batch, include public dummy queries and their costs;
verify their output as well. If a protocol pads the database, its extra records
are zero-filled and counted separately. Root must actually be retrieved, not
returned from the verification oracle. Real DB setup/hints are required.

## Result format

Top-level fields:

- `schema_version`: `path-baseline-v1`
- `scheme`: `spirit`, `spirit_rc`, `vbpir_pbc`, `inspire_layerwise`, `treepir_spiral`, or `respire`
- `path_mode`: `ancestor_root_included`
- `height`, `leaf_count`, `record_bytes`(32), `logical_records`, `logical_db_bytes`
- `backend_records`, `batch_size`: effective padded geometry
- `warmups`, `trials`: requested counts
- `source_commit`: exact upstream revision; wrapper revision may be separate
- `parameters`: complete protocol-specific crypto/layout parameters
- `setup`: independent offline timings (e.g. `preprocess_ms`, `keygen_ms`) and helper bytes
- `communication`: labels for `query` and `response`: `actual_serialized` or an explicit model name
- `profiling_coverage`: `exclusive_stages` or `server_total_only`
- `warmup_samples`: W validated samples
- `samples`: N validated measured samples

Each sample contains:

```
{"leaf": 6, "server_ms": 1.0, "query_ms": 0.2, "decode_ms": 0.1,
 "query_bytes": 1024, "response_bytes": 1024,
 "correct": true, "correct_entries": 4, "path_entries": 4,
 "stages_ms": {"scan": 0.7, "response_compression": 0.3}}
```

The example numbers only illustrate the schema. Actual stage names are native
to the protocol and must be mutually exclusive. Preserve uninstrumented stages
as unavailable, not invented zeros. `server_ms` covers the entire online server
answer for one full path, including all color buckets or per-level requests.
Timing excludes verification; query/decode timings measure the actual client.
Responses and queries must use actual serialization when available. Model-only
sizes must be labeled and must not be combined with actual byte measurements.
Failure must return a nonzero process status and preserve diagnostics, rather
than emitting `correct:true` or usable timings for an incomplete sample.

## Isolation and validation

Keep upstream checkouts and builds under ignored `.benchmarks/`; retain only
narrow source/patch/build instructions and tests here. `docs/` and local backups
are excluded from archives/pushes. Small encrypted tests precede formal runs.
H24/32B is the initial formal target, 3 warmups and 100 measured trials; the
TreePIR+Spiral point uses 16 measured trials from the same schedule prefix.

## Finite campaign runner

Run selected, already-built adapters sequentially:

```
python3 scripts/run_path_baselines.py --height 24 --warmup 3 --trials 100 \
  --treepir-trials 16 --seed 7 --output outputs/path-baselines-20260914/formal \
  --binary spirit=/absolute/path/spirit_path_adapter \
  --binary vbpir_pbc=/absolute/path/vbpir_path_adapter
```

Repeat `--binary` for the other requested modes once their encrypted smoke tests
pass. SPIRIT and SPIRIT+RC may share a binary; the runner supplies `--scheme`.
Only supplied modes are executed, so a partial selection is not the complete
six-mode campaign. A missing executable is a launch error; a nonzero process or
invalid report is preserved as a failed run and excluded from summary means.
The runner creates a fresh output folder, records peak process RSS, and runs
adapters from that folder so upstream temporary files do not pollute the repo.
