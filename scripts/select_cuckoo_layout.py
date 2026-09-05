#!/usr/bin/env python3
"""Select the fastest valid Cuckoo expansion-height pilot."""

from __future__ import annotations

import argparse
import json
import math
import statistics
import sys
from pathlib import Path
from typing import Any


class SelectionError(Exception):
    pass


def load_json(path: Path) -> dict[str, Any]:
    try:
        data = json.loads(path.read_text())
    except FileNotFoundError as exc:
        raise SelectionError(f"{path}: missing pilot JSON") from exc
    except json.JSONDecodeError as exc:
        raise SelectionError(f"{path}: invalid JSON: {exc}") from exc
    if not isinstance(data, dict):
        raise SelectionError(f"{path}: pilot JSON is not an object")
    return data


def require_int(data: dict[str, Any], key: str, path: Path) -> int:
    value = data.get(key)
    if not isinstance(value, int) or isinstance(value, bool):
        raise SelectionError(f"{path}: {key} is not an integer")
    return value


def finite_nonnegative_ms(value: Any, path: Path, label: str) -> float:
    if not isinstance(value, (int, float)) or isinstance(value, bool):
        raise SelectionError(f"{path}: {label} is not numeric")
    result = float(value)
    if not math.isfinite(result) or result < 0.0:
        raise SelectionError(f"{path}: {label} is not finite non-negative ms")
    return result


def bucket_shape(data: dict[str, Any]) -> list[tuple[int, int]]:
    layouts = data.get("bucket_layouts")
    if not isinstance(layouts, list):
        return []
    shape: list[tuple[int, int]] = []
    for entry in layouts:
        if not isinstance(entry, dict):
            return []
        bucket = entry.get("bucket")
        members = entry.get("member_count")
        if not isinstance(bucket, int) or not isinstance(members, int):
            return []
        shape.append((bucket, members))
    return shape


def validate_pilot(
    path: Path,
    expected_trials: int,
    expected_warmups: int,
    expected_leaf_count: int | None,
) -> dict[str, Any]:
    data = load_json(path)
    process_path = path.with_name(f"{path.stem}-process.json")
    process = load_json(process_path)
    if process.get("exit_code") != 0:
        raise SelectionError(f"{process_path}: exit_code is not 0")
    height = require_int(data, "requested_expansion_height", path)
    if data.get("layout_policy") != "forced_expansion_height":
        raise SelectionError(f"{path}: layout_policy is not forced_expansion_height")
    if require_int(data, "measured_trials", path) != expected_trials:
        raise SelectionError(f"{path}: measured_trials != {expected_trials}")
    if require_int(data, "warmups", path) != expected_warmups:
        raise SelectionError(f"{path}: warmups != {expected_warmups}")
    if expected_leaf_count is not None and require_int(data, "leaf_count", path) != expected_leaf_count:
        raise SelectionError(f"{path}: leaf_count != {expected_leaf_count}")

    samples = data.get("samples")
    if not isinstance(samples, list) or len(samples) != expected_trials:
        raise SelectionError(f"{path}: samples count is not {expected_trials}")
    leaf_schedule: list[int] = []
    server_samples: list[float] = []
    for idx, sample in enumerate(samples):
        if not isinstance(sample, dict):
            raise SelectionError(f"{path}: sample {idx} is not an object")
        if sample.get("correct") is not True:
            raise SelectionError(f"{path}: sample {idx} failed correctness")
        leaf = sample.get("leaf")
        if not isinstance(leaf, int) or isinstance(leaf, bool):
            raise SelectionError(f"{path}: sample {idx} leaf is not an integer")
        leaf_schedule.append(leaf)
        server_samples.append(
            finite_nonnegative_ms(sample.get("server_total_ms"), path, f"sample {idx} server_total_ms")
        )

    reference_layout = data.get("reference_layout")
    if not isinstance(reference_layout, dict):
        raise SelectionError(f"{path}: missing reference_layout")
    buckets = bucket_shape(data)
    if not buckets:
        raise SelectionError(f"{path}: missing bucket member shape")

    storage = data.get("storage")
    if not isinstance(storage, dict):
        raise SelectionError(f"{path}: missing storage")
    communication = data.get("communication")
    if not isinstance(communication, dict):
        raise SelectionError(f"{path}: missing communication")

    physical_db_bytes = storage.get("estimated_preprocessed_bytes")
    if not isinstance(physical_db_bytes, (int, float)) or isinstance(physical_db_bytes, bool):
        raise SelectionError(f"{path}: missing physical DB byte estimate")

    return {
        "path": str(path),
        "height": height,
        "median_server_ms": statistics.median(server_samples),
        "mean_server_ms": statistics.fmean(server_samples),
        "server_samples_ms": server_samples,
        "leaf_schedule": leaf_schedule,
        "reference_layout": reference_layout,
        "bucket_inputs": {
            "bucket_sizing": data.get("bucket_sizing"),
            "batch_size": data.get("batch_size"),
            "num_buckets": data.get("num_buckets"),
            "num_hashes": data.get("num_hashes"),
            "bucket_members": buckets,
        },
        "seeds": {
            "hash_seed": data.get("hash_seed"),
            "trial_seed": data.get("trial_seed"),
        },
        "storage": storage,
        "communication": communication,
        "physical_db_bytes": float(physical_db_bytes),
    }


def validate_same_inputs(pilots: list[dict[str, Any]]) -> None:
    first = pilots[0]
    for pilot in pilots[1:]:
        for key, label in (
            ("leaf_schedule", "measured leaf schedule"),
            ("reference_layout", "reference layout"),
            ("bucket_inputs", "bucket/hash-bucket shape"),
            ("seeds", "hash/trial seeds"),
        ):
            if pilot[key] != first[key]:
                raise SelectionError(
                    f"{pilot['path']}: {label} does not match {first['path']}"
                )


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("pilots", type=Path, nargs="+", help="pilot JSON files")
    parser.add_argument("--output", type=Path, required=True, help="selection JSON path")
    parser.add_argument("--expected-trials", type=int, default=16)
    parser.add_argument("--expected-warmups", type=int, default=3)
    parser.add_argument("--leaf-count", type=int)
    parser.add_argument("--heights", default="7,8,9,10")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    expected_heights = {int(part) for part in args.heights.split(",") if part}
    try:
        pilots = [
            validate_pilot(path, args.expected_trials, args.expected_warmups, args.leaf_count)
            for path in args.pilots
        ]
        found_heights = {pilot["height"] for pilot in pilots}
        if found_heights != expected_heights or len(pilots) != len(expected_heights):
            raise SelectionError(
                f"pilot heights {sorted(found_heights)} != expected {sorted(expected_heights)}"
            )
        validate_same_inputs(pilots)
        ordered = sorted(
            pilots,
            key=lambda p: (p["mean_server_ms"], p["physical_db_bytes"], p["height"]),
        )
        chosen = ordered[0]
        result = {
            "schema_version": "cuckoo_layout_selection.v1",
            "decision": {
                "score": "min_mean_complete_path_server_ms",
                "tie_breakers": ["physical_db_bytes", "requested_expansion_height"],
                "chosen_expansion_height": chosen["height"],
                "chosen_pilot": chosen["path"],
            },
            "pilots": [
                {
                    "path": pilot["path"],
                    "requested_expansion_height": pilot["height"],
                    "median_server_ms": pilot["median_server_ms"],
                    "mean_server_ms": pilot["mean_server_ms"],
                    "physical_db_bytes": pilot["physical_db_bytes"],
                    "communication": pilot["communication"],
                    "storage": pilot["storage"],
                }
                for pilot in sorted(pilots, key=lambda p: p["height"])
            ],
        }
        args.output.parent.mkdir(parents=True, exist_ok=True)
        args.output.write_text(json.dumps(result, indent=2, sort_keys=True) + "\n")
        print(chosen["height"])
        return 0
    except (SelectionError, ValueError) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
