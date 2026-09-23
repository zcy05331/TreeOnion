# Retained full-path PIR baselines

This directory keeps only the external path baselines that remain part of the
final comparison set.

| Mode | Implementation and build instructions |
|---|---|
| `spirit_rc` | [spirit/](spirit/README.md): public SPIRIT batch PIR with encrypted response compression |
| `respire` | [respire/](respire/README.md): native Respire batch path workload |

The removed standalone SPIRIT, VBPIR+PBC, InsPIRe and TreePIR+Spiral prototypes
are preserved on `codex/archive-full-pir-20260920` at `fcc521f`.

The legacy-path adapters below retrieve the root-excluded Merkle path of the
OnionPIR benchmarks (seed-7 leaves) and have their own runners and JSON schemas
instead of the CLI described in the rest of this file:

| Adapter | Scheme | Runner |
|---|---|---|
| [spiral_pbc/](spiral_pbc/README.md) | Spiral behind the OnionPIR cuckoo baseline's PBC instance | `scripts/run_spiral_pbc_matrix.py` |
| [kspir_pbc/](kspir_pbc/README.md) | KsPIR behind the same PBC instance | `scripts/run_kspir_pbc_matrix.py` |
| [via_b/](via_b/README.md) | VIA-B as in the paper (native), or per PBC bucket (`--batch pbc`) | `scripts/run_via_b_matrix.py` |

## Workload and CLI

Every retained adapter accepts:

```text
<adapter> --height H --leaf-file leaves.txt --warmup W --trials N --output result.json
```

Leaves are zero-based ASCII integers, one per line. Consume exactly the first
`W+N`; warmups are a separate prefix. One sample returns all `H+1` ancestors,
including root, in root-to-leaf order. A complete binary tree has
`(1<<(H+1))-1` records. BFS record index for level `l` and local index `j` is
`(1<<l)-1+j`.

Data is exactly the existing `synthetic_tree_node_bytes` source in
`src/tree_database.cpp`. Shared workload helpers live in `workload.py` and
`common/path_workload.h`.

Golden record hex:

- `(level0,index0)`: `b8782335a8e76c4bf81ce235c978f777b654f924299739320bda3618dcc37790`
- `(level3,index6)`: `3849527f67cb7a2c02bb1678b692eb093d424f3e552d646e20c3159427402066`
- `(level24,index16777215)`: `aa4765a84a38d7d577d573482b756942f01ad5caf1c03c7adfec59dcda06b3cd`

If a protocol pads the batch, include public dummy queries and their costs;
verify their output as well. If a protocol pads the database, its extra records
are zero-filled and counted separately. Root must be retrieved by the protocol,
not returned from the verification oracle.

## Result format

Top-level fields:

- `schema_version`: `path-baseline-v1`
- `scheme`: `spirit_rc` or `respire`
- `path_mode`: `ancestor_root_included`
- `height`, `leaf_count`, `record_bytes` (32), `logical_records`, `logical_db_bytes`
- `backend_records`, `batch_size`
- `warmups`, `trials`
- `source_commit`
- `parameters`
- `setup`
- `communication`: labels for `query` and `response`
- `profiling_coverage`: `exclusive_stages` or `server_total_only`
- `warmup_samples`, `samples`

Each sample contains complete online server timing for one full path plus query,
decode, communication, correctness and native stage fields. Failure must return
a nonzero process status and preserve diagnostics instead of emitting usable
timings for an incomplete sample.

## Finite campaign runner

Run selected, already-built adapters sequentially:

```bash
python3 scripts/run_path_baselines.py --height 24 --warmup 3 --trials 50 \
  --seed 7 --output outputs/path-baselines/formal \
  --binary spirit_rc=/absolute/path/spirit_path_adapter \
  --binary respire=/absolute/path/respire_path_workload.py
```

Only supplied modes are executed. A missing executable is a launch error; a
nonzero process or invalid report is preserved as a failed run and excluded from
summary means.
