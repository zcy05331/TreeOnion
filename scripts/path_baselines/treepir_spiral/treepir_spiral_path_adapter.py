#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import os
import time
import sys
from pathlib import Path
import math

try:
    from .spiral_runner import DEFAULT_PRESET, BucketRun, SpiralPreset, run_spiral_bucket
    from .subcsa import (
        COLORS,
        balanced_color_sequence,
        balanced_color_sizes,
        color_index,
        color_path_positions,
        feasible_split,
        local_csa_indices,
    )
    from .synthetic_tree import node_bytes, path_indices, path_level_indices, read_leaves
except ImportError:
    from spiral_runner import DEFAULT_PRESET, BucketRun, SpiralPreset, run_spiral_bucket
    from subcsa import (
        COLORS,
        balanced_color_sequence,
        balanced_color_sizes,
        color_index,
        color_path_positions,
        feasible_split,
        local_csa_indices,
    )
    from synthetic_tree import node_bytes, path_indices, path_level_indices, read_leaves


SPIRAL_HASHES_PER_RECORD = 256


def select_spiral_preset(logical_records: int) -> SpiralPreset:
    required_records = max(1, math.ceil(logical_records / SPIRAL_HASHES_PER_RECORD))
    best: tuple[int, int, int] | None = None
    for num_expansions in range(1, 10):
        for further_dims in range(1, 8):
            total_records = 1 << (num_expansions + further_dims)
            if total_records < required_records:
                continue
            candidate = (total_records, num_expansions, further_dims)
            if best is None or candidate < best:
                best = candidate
    if best is None:
        raise ValueError(f"bucket with {logical_records} records exceeds staged Spiral shape search")
    return SpiralPreset(str(best[1]), str(best[2]), DEFAULT_PRESET.database_name, DEFAULT_PRESET.argv4)


def _mean(values: list[float]) -> float:
    return sum(values) / len(values) if values else 0.0


def find_spiral_manifest(spiral_bin: Path) -> Path | None:
    for parent in [spiral_bin.parent, *spiral_bin.parents]:
        candidate = parent / "manifest.json"
        if candidate.exists():
            return candidate
    return None


def load_spiral_manifest(spiral_bin: Path) -> dict:
    manifest_path = find_spiral_manifest(spiral_bin)
    if manifest_path is None:
        return {
            "source_commit": "fixture:fake_spiral_no_manifest",
            "source_manifest": None,
            "manifest": None,
        }
    manifest = json.loads(manifest_path.read_text(encoding="utf-8"))
    commit = manifest.get("upstream_treepir_commit")
    if not commit:
        raise SystemExit(f"Spiral manifest missing upstream_treepir_commit: {manifest_path}")
    return {
        "source_commit": commit,
        "source_manifest": str(manifest_path),
        "manifest": manifest,
    }


def _empty_bucket_records(height: int) -> list[list[bytes | None]]:
    return [[None for _ in range(size)] for size in balanced_color_sizes(height)]


def canonical_bucket_records(height: int) -> list[list[bytes]]:
    records = _empty_bucket_records(height)
    next_index = [0] * height

    def put(color: str, level: int, local: int) -> None:
        bucket = color_index(color)
        local_index = next_index[bucket]
        if local_index >= len(records[bucket]):
            raise ValueError(f"color {color} overflowed canonical bucket size {len(records[bucket])}")
        records[bucket][local_index] = node_bytes(level, local)
        next_index[bucket] += 1

    def visit(subtree_height: int, colors, parent_level: int, parent_local: int) -> None:
        left_level = parent_level + 1
        left_local = parent_local * 2
        right_local = left_local + 1
        if subtree_height == 1:
            put(colors[0].color, left_level, left_local)
            put(colors[0].color, left_level, right_local)
            return

        split_point = 2 if colors[0].size == 2 else 1
        put(colors[0].color, left_level, left_local)
        put(colors[0 if split_point == 2 else 1].color, left_level, right_local)
        left_colors, right_colors = feasible_split(subtree_height, colors)
        visit(subtree_height - 1, left_colors, left_level, left_local)
        visit(subtree_height - 1, right_colors, left_level, right_local)

    visit(height, balanced_color_sequence(height), 0, 0)
    finalized: list[list[bytes]] = []
    for color, bucket in enumerate(records):
        missing = [index for index, value in enumerate(bucket) if value is None]
        if missing:
            raise ValueError(f"color {COLORS[color]} has {len(missing)} unfilled canonical slots")
        finalized.append([value for value in bucket if value is not None])
    return finalized


def _root_record() -> bytes:
    return node_bytes(0, 0)


def build_schedule(height: int, warmup_leaves: list[int], measured_leaves: list[int]) -> list[dict]:
    rows = []
    row_id = 0
    for phase, leaves in (("warmup", warmup_leaves), ("measured", measured_leaves)):
        for leaf in leaves:
            local_indices = local_csa_indices(height, leaf)
            rows.append({
                "row_id": row_id,
                "phase": phase,
                "leaf": leaf,
                "path_indices": path_indices(height, leaf),
                "path_level_indices": path_level_indices(height, leaf),
                "color_local_indices": local_indices,
                "color_path_positions": color_path_positions(height, leaf),
                "root_local_index": 0,
            })
            row_id += 1
    return rows


def run_adapter(args: argparse.Namespace) -> dict:
    warmup_leaves, measured_leaves = read_leaves(
        args.leaf_file,
        height=args.height,
        warmup=args.warmup,
        trials=args.trials,
    )
    rows = build_schedule(args.height, warmup_leaves, measured_leaves)
    if args.validate_mapping_only:
        bucket_runs: list[BucketRun] = []
    else:
        spiral_bin = args.spiral_bin or os.environ.get("TREEPIR_SPIRAL_BIN")
        if not spiral_bin:
            raise SystemExit("--spiral-bin or TREEPIR_SPIRAL_BIN is required for encrypted Spiral runs")
        spiral_bin_path = Path(spiral_bin).resolve()
        if not spiral_bin_path.is_file():
            raise SystemExit(f"Spiral binary not found: {spiral_bin_path}")
        spiral_metadata = load_spiral_manifest(spiral_bin_path)
        workspace_parent = args.work_root.resolve()
        bucket_runs = []
        print("preparing canonical TreePIR color databases", file=sys.stderr, flush=True)
        records_by_color = canonical_bucket_records(args.height)
        all_leaves = warmup_leaves + measured_leaves
        for color in range(args.height):
            print(f"running color {color + 1}/{args.height}", file=sys.stderr, flush=True)
            query_indices = [local_csa_indices(args.height, leaf)[color] for leaf in all_leaves]
            bucket_runs.append(run_spiral_bucket(
                spiral_bin=spiral_bin_path,
                label=f"color{COLORS[color]}",
                records=records_by_color[color],
                query_indices=query_indices,
                workspace_parent=workspace_parent,
                preset=select_spiral_preset(len(records_by_color[color])),
                timeout=args.timeout,
                keep_work_dir=args.keep_work_dirs,
            ))
        bucket_runs.append(run_spiral_bucket(
            spiral_bin=spiral_bin_path,
            label="root",
            records=[_root_record()],
            query_indices=[0] * len(all_leaves),
            workspace_parent=workspace_parent,
            preset=select_spiral_preset(1),
            timeout=args.timeout,
            keep_work_dir=args.keep_work_dirs,
        ))

    if args.validate_mapping_only:
        spiral_metadata = {"source_commit": "validate_mapping_only", "source_manifest": None, "manifest": None}

    run_by_label = {run.label: run for run in bucket_runs}
    row_positions = {id(row): position for position, row in enumerate(rows)}

    def make_sample(row: dict) -> dict:
        query_position = row_positions[id(row)]
        per_bucket = []
        query_ms = server_ms = decode_ms = query_bytes = response_bytes = 0.0
        for color in range(args.height):
            print(f"running color {color + 1}/{args.height}", file=sys.stderr, flush=True)
            run = run_by_label.get(f"color{COLORS[color]}")
            local_index = row["color_local_indices"][color]
            level = row["color_path_positions"][color]
            local = dict(row["path_level_indices"])[level]
            entry = node_bytes(level, local).hex()
            if run:
                query_ms += run.metric_ms_at("Query_Generation.client", query_position)
                server_ms += run.metric_ms_at("Answer.server", query_position)
                decode_ms += run.metric_ms_at("Extract_Response.client", query_position)
                query_bytes += run.query_bytes
                response_bytes += run.response_bytes
            per_bucket.append({
                "bucket": f"color{COLORS[color]}",
                "run_label": f"color{COLORS[color]}",
                "path_position": level,
                "local_index": local_index,
                "path_level": level,
                "path_local_index": local,
                "expected_hex": entry,
                "pir_verified": bool(run and run.correct_by_query[query_position]),
            })
        root_run = run_by_label.get("root")
        if root_run:
            query_ms += root_run.metric_ms_at("Query_Generation.client", query_position)
            server_ms += root_run.metric_ms_at("Answer.server", query_position)
            decode_ms += root_run.metric_ms_at("Extract_Response.client", query_position)
            query_bytes += root_run.query_bytes
            response_bytes += root_run.response_bytes
        per_bucket.insert(0, {
            "bucket": "root",
            "run_label": "root",
            "path_position": 0,
            "local_index": 0,
            "path_level": 0,
            "path_local_index": 0,
            "expected_hex": _root_record().hex(),
            "pir_verified": bool(root_run and root_run.correct_by_query[query_position]),
        })
        return {
            "leaf": row["leaf"],
            "phase": row["phase"],
            "path_indices": row["path_indices"],
            "decoded_path_indices": row["path_indices"] if bucket_runs else [],
            "path_records": per_bucket,
            "correct": bool(bucket_runs) and all(item["pir_verified"] for item in per_bucket),
            "correct_entries": len(per_bucket) if bucket_runs else 0,
            "path_entries": args.height + 1,
            "query_ms": query_ms,
            "server_ms": server_ms,
            "decode_ms": decode_ms,
            "query_bytes": int(query_bytes),
            "response_bytes": int(response_bytes),
            "stages_ms": {},
        }

    warmup_samples = [make_sample(row) for row in rows if row["phase"] == "warmup"]
    samples = [make_sample(row) for row in rows if row["phase"] == "measured"]

    result = {
        "schema_version": "path-baseline-v1",
        "scheme": "treepir_spiral",
        "path_mode": "ancestor_root_included",
        "height": args.height,
        "leaf_count": 1 << args.height,
        "record_bytes": 32,
        "logical_records": (1 << (args.height + 1)) - 1,
        "logical_db_bytes": ((1 << (args.height + 1)) - 1) * 32,
        "backend_records": sum(balanced_color_sizes(args.height)) + 1,
        "batch_size": args.height + 1,
        "path_length": args.height + 1,
        "warmups": args.warmup,
        "trials": args.trials,
        "source_commit": spiral_metadata["source_commit"],
        "source_manifest": spiral_metadata["source_manifest"],
        "leaf_schedule": {"warmup": warmup_leaves, "measured": measured_leaves},
        "parameters": {
            "colors": COLORS[:args.height],
            "spiral_database_arg": DEFAULT_PRESET.database_name,
            "spiral_hashes_per_record": SPIRAL_HASHES_PER_RECORD,
            "spiral_crypto_build": (spiral_metadata["manifest"] or {}).get("preset", {}).get("build", {
                "fixture": True,
            }),
            "root_bucket_entries": 1,
        },
        "setup": {
            "preprocess_ms": None,
            "keygen_ms": None,
            "helper_bytes": None,
            "note": "upstream Spiral reports setup inside each external bucket process; adapter preserves raw bucket metrics instead of inventing reusable full-path setup zeros",
        },
        "root_handling": {
            "mode": "encrypted_constant_bucket",
            "reason": "upstream CSA colors exclude the root; the adapter serves root as a one-record Spiral bucket instead of plaintext",
        },
        "communication": {
            "query": "spiral_static_model_Query_Cost.communication",
            "response": "spiral_static_model_Response_Cost.communication",
            "public_batch_padding": 0,
        },
        "profiling_coverage": "server_total_only",
        "layout": {
            "non_root_color_buckets": args.height,
            "root_bucket_entries": 1,
            "color_bucket_sizes": balanced_color_sizes(args.height),
            "spiral_shape_policy": "smallest total_n=2^(num_expansions+further_dims) with num_expansions>=1 and further_dims>=1 covering ceil(bucket_records/256)",
        },
        "run_mode": "validate_mapping_only" if args.validate_mapping_only else "external_spiral_encrypted",
        "measured_summary": {
            "count": len(samples),
            "query_ms_mean": _mean([s["query_ms"] for s in samples]),
            "server_ms_mean": _mean([s["server_ms"] for s in samples]),
            "decode_ms_mean": _mean([s["decode_ms"] for s in samples]),
            "query_bytes_mean": _mean([s["query_bytes"] for s in samples]),
            "response_bytes_mean": _mean([s["response_bytes"] for s in samples]),
            "all_correct": all(s["correct"] for s in samples),
        },
        "warmup_samples": warmup_samples,
        "samples": samples,
        "bucket_runs": [
            {
                "bucket": run.label,
                "query_count": len(run.query_indices),
                "correct_count": run.correct_count,
                "correct_by_query": run.correct_by_query,
                "work_dir": run.work_dir,
                "elapsed_ms": run.elapsed_ms,
                "record_count": run.record_count,
                "spiral_num_expansions": int(run.preset.num_expansions),
                "spiral_further_dims": int(run.preset.further_dims),
                "spiral_total_records": 1 << (int(run.preset.num_expansions) + int(run.preset.further_dims)),
                "spiral_capacity_hashes": (1 << (int(run.preset.num_expansions) + int(run.preset.further_dims))) * SPIRAL_HASHES_PER_RECORD,
                "query_ms_total": run.query_ms,
                "server_ms_total": run.server_ms,
                "decode_ms_total": run.decode_ms,
                "query_bytes": run.query_bytes,
                "response_bytes": run.response_bytes,
                "metric_names": sorted(run.metrics),
                "metric_value_counts": {name: len(values) for name, values in sorted(run.metrics.items())},
            }
            for run in bucket_runs
        ],
    }
    return result


def main() -> int:
    parser = argparse.ArgumentParser(description="Run TreePIR+Spiral as one encrypted lookup per path bucket.")
    parser.add_argument("--height", type=int, required=True)
    parser.add_argument("--leaf-file", type=Path, required=True)
    parser.add_argument("--warmup", type=int, required=True)
    parser.add_argument("--trials", type=int, required=True)
    parser.add_argument("--output", type=Path, required=True)
    parser.add_argument("--spiral-bin", type=Path)
    parser.add_argument("--work-root", type=Path, default=Path("/tmp/treepir-spiral-path-adapter"))
    parser.add_argument("--timeout", type=int, default=3600)
    parser.add_argument("--keep-work-dirs", action="store_true")
    parser.add_argument("--validate-mapping-only", action="store_true")
    args = parser.parse_args()
    start = time.perf_counter()
    data = run_adapter(args)
    data["adapter_wall_ms"] = (time.perf_counter() - start) * 1000.0
    args.output.parent.mkdir(parents=True, exist_ok=True)
    args.output.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
