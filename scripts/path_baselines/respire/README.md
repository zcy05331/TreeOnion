# Respire Path Adapter

This adapter stages upstream Respire commit `fd20d38e9e94c0298ab21af2f7699a23729eef67` under `.benchmarks/respire-path/respire`, applies `patches/respire_timing_boundaries.patch`, and copies the `path_workload/` Rust module folder into `src/bin/path_workload/` for the shared full ancestor path workload.

Formal run:

```bash
python3 scripts/path_baselines/respire/respire_path_workload.py \
  --height 24 \
  --leaf-file outputs/profiling/1gb/leaves.txt \
  --warmup 3 \
  --trials 100 \
  --output outputs/profiling/1gb/respire-path.json
```

The formal type is `CuckooRespireImpl<32,49,2^25>` with the controlled 32-byte, 1 GiB parameters from the RPM measurement (`batch_256(49,8,8,8)`, `D3=64`). The height-8 type uses the same record size and batch geometry with `NU1=NU2=1` so local encrypted smoke runs have bounded memory.

Use `--dry-run` to inspect the shared path schedule and plaintext goldens without building Respire.

To stage without running:

```bash
bash scripts/path_baselines/respire/stage_respire.sh
```

Set `RESPIRE_SOURCE_DIR=/path/to/respire` to stage from an existing checkout instead of cloning. The source must be a clean git checkout whose `HEAD` is exactly `fd20d38e9e94c0298ab21af2f7699a23729eef67`; otherwise staging rejects it.
