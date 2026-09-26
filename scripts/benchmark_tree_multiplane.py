#!/usr/bin/env python3
"""Tune N0 after fixing the h1 layout by utilization and P<=6."""

from __future__ import annotations

import argparse
import json
import math
import os
import sys
from dataclasses import dataclass
from pathlib import Path
from statistics import fmean
from typing import Any

from read_process_peak_memory import run_measured
from summarize_cloudlab_profiles import ProfileError, validate_sampled


class SweepError(Exception):
    pass


def select_layout(entry_bits: int, chunk_bits: int = 12,
                  ring_degree: int = 2048) -> tuple[int, int]:
    if entry_bits <= 0 or chunk_bits <= 0 or ring_degree < 2 or ring_degree & (ring_degree - 1):
        raise SweepError("layout needs positive payload widths and a power-of-two ring")
    choices = []
    g = 1
    while g <= ring_degree // 2:
        planes = (entry_bits + chunk_bits * g - 1) // (chunk_bits * g)
        if planes <= 6:
            choices.append((g * planes, planes, g))
        g *= 2
    if not choices:
        raise SweepError("entry cannot fit in six planes with rho >= 2")
    _, planes, g = min(choices)
    return g, planes


@dataclass(frozen=True)
class Case:
    label: str
    g: int
    n0: int
    raw_json: Path
    process_json: Path
    log: Path


def positive_power_of_two(value: str, flag: str) -> int:
    try:
        parsed = int(value)
    except ValueError as exc:
        raise argparse.ArgumentTypeError(f"{flag} must contain integers") from exc
    if parsed <= 0 or parsed & (parsed - 1):
        raise argparse.ArgumentTypeError(f"{flag} values must be positive powers of two")
    return parsed


def parse_power2_csv(value: str, flag: str) -> list[int]:
    if not value.strip():
        raise argparse.ArgumentTypeError(f"{flag} must not be empty")
    parsed = [positive_power_of_two(part.strip(), flag) for part in value.split(",")]
    if len(set(parsed)) != len(parsed):
        raise argparse.ArgumentTypeError(f"{flag} values must be unique")
    return parsed


def finite_nonnegative(value: Any, path: Path, name: str) -> float:
    if not isinstance(value, (int, float)) or isinstance(value, bool):
        raise SweepError(f"{path}: {name} is not numeric")
    result = float(value)
    if not math.isfinite(result) or result < 0.0:
        raise SweepError(f"{path}: {name} is not a finite non-negative timing")
    return result


def metadata_int(metadata: dict[str, Any], key: str, path: Path) -> int:
    value = metadata.get(key)
    if isinstance(value, str):
        try:
            value = int(value)
        except ValueError as exc:
            raise SweepError(f"{path}: metadata.{key} is not an integer") from exc
    if not isinstance(value, int) or isinstance(value, bool):
        raise SweepError(f"{path}: metadata.{key} is not an integer")
    return value


def metadata_str(metadata: dict[str, Any], key: str, path: Path) -> str:
    value = metadata.get(key)
    if value is None:
        raise SweepError(f"{path}: missing metadata.{key}")
    return str(value)


def validate_result(path: Path, *, case: Case, leaf_count: int, entry_bits: int,
                    experiments: int, warmup: int, packing: str,
                    query_layout: str, trial_seed: int,
                    policy_layout: bool = True) -> dict[str, Any]:
    """Check one benchmark JSON. policy_layout=False admits a plane geometry
    outside the P<=6 policy (the plane-geometry ablation); every other check,
    including the minimal plane count for the row's g, still applies."""
    try:
        data = json.loads(path.read_text())
    except FileNotFoundError as exc:
        raise SweepError(f"{path}: missing result JSON") from exc
    except json.JSONDecodeError as exc:
        raise SweepError(f"{path}: invalid JSON: {exc}") from exc
    if not isinstance(data, dict):
        raise SweepError(f"{path}: result JSON is not an object")
    if data.get("scheme") != "tree-multiplane":
        raise SweepError(f"{path}: scheme is not tree-multiplane")
    if data.get("leaf_count") != leaf_count:
        raise SweepError(f"{path}: leaf_count {data.get('leaf_count')} != expected {leaf_count}")
    if data.get("node_entry_bits") != entry_bits:
        raise SweepError(f"{path}: node_entry_bits {data.get('node_entry_bits')} != expected {entry_bits}")
    if data.get("measured_trials") != experiments:
        raise SweepError(f"{path}: measured trial count != expected {experiments}")
    if data.get("warmups") != warmup:
        raise SweepError(f"{path}: warmup count {data.get('warmups')} != expected {warmup}")
    if data.get("trial_seed") != trial_seed:
        raise SweepError(f"{path}: trial_seed {data.get('trial_seed')} != expected {trial_seed}")
    if data.get("mode") != packing:
        raise SweepError(f"{path}: mode {data.get('mode')!r} != expected {packing!r}")

    metadata = data.get("metadata")
    if not isinstance(metadata, dict):
        raise SweepError(f"{path}: missing metadata object")
    expected_strings = {
        "index_layout": "gamma-alpha-beta",
        "h": "1",
        "response_packing": packing,
        "query_layout": query_layout,
    }
    for key, expected in expected_strings.items():
        actual = metadata_str(metadata, key, path)
        if actual != expected:
            raise SweepError(f"{path}: metadata.{key} {actual!r} != expected {expected!r}")
    for key, expected in (
        ("g", case.g),
        ("N0", case.n0),
    ):
        actual = metadata_int(metadata, key, path)
        if actual != expected:
            raise SweepError(f"{path}: metadata.{key} {actual} != expected {expected}")
    ring_degree = metadata_int(metadata, "ring_degree", path)
    chunk_bits = metadata_int(metadata, "chunk_bits", path)
    if ring_degree <= 0 or case.g <= 0 or ring_degree % case.g != 0:
        raise SweepError(f"{path}: metadata.ring_degree must be positive and divisible by g")
    if chunk_bits <= 0:
        raise SweepError(f"{path}: metadata.chunk_bits must be positive")
    plane_count = metadata_int(metadata, "plane_count", path)
    if plane_count <= 0:
        raise SweepError(f"{path}: metadata.plane_count must be positive")
    b = metadata_int(metadata, "b", path)
    if b < 0:
        raise SweepError(f"{path}: metadata.b must be non-negative")
    expected_planes = (entry_bits + chunk_bits * case.g - 1) // (chunk_bits * case.g)
    if plane_count != expected_planes:
        raise SweepError(f"{path}: metadata.plane_count {plane_count} != minimal {expected_planes}")
    if policy_layout and (case.g, plane_count) != select_layout(entry_bits, chunk_bits, ring_degree):
        raise SweepError(f"{path}: layout violates maximum utilization, minimum P, P<=6 policy")
    rho = ring_degree // case.g
    reported_rho = metadata_int(metadata, "rho", path)
    if reported_rho != rho:
        raise SweepError(f"{path}: metadata.rho {reported_rho} != ring_degree/g {rho}")
    denominator = rho * case.n0
    if denominator <= 0 or leaf_count % denominator != 0:
        raise SweepError(f"{path}: leaf_count is not rho*N0*2^b")
    expansion = leaf_count // denominator
    if expansion <= 0 or expansion & (expansion - 1):
        raise SweepError(f"{path}: leaf_count/(rho*N0) is not a power of two")
    expected_b = expansion.bit_length() - 1
    if b != expected_b:
        raise SweepError(f"{path}: metadata.b {b} != derived {expected_b}")

    samples = data.get("samples")
    if not isinstance(samples, list):
        raise SweepError(f"{path}: missing samples")
    if len(samples) != experiments:
        raise SweepError(f"{path}: samples count {len(samples)} != expected {experiments}")
    leaves: list[int] = []
    server_times: list[float] = []
    for idx, sample in enumerate(samples):
        if not isinstance(sample, dict):
            raise SweepError(f"{path}: sample {idx} is not an object")
        if sample.get("correct") is not True:
            raise SweepError(f"{path}: sample {idx} failed correctness")
        if sample.get("trial") != idx:
            raise SweepError(f"{path}: sample {idx} trial sequence mismatch")
        leaf = sample.get("leaf")
        if not isinstance(leaf, int) or isinstance(leaf, bool):
            raise SweepError(f"{path}: sample {idx} missing integer leaf")
        if leaf < 0 or leaf >= leaf_count:
            raise SweepError(f"{path}: sample {idx} leaf out of range")
        leaves.append(leaf)
        server_times.append(finite_nonnegative(sample.get("server_total_ms"), path, f"sample[{idx}].server_total_ms"))
        stages = sample.get("stages_ms")
        if not isinstance(stages, dict) or not stages:
            raise SweepError(f"{path}: sample {idx} missing stages_ms")
        stage_sum = 0.0
        for stage, value in stages.items():
            stage_sum += finite_nonnegative(value, path, f"sample[{idx}].stages_ms.{stage}")
        if stage_sum > sample["server_total_ms"] + 1e-6:
            raise SweepError(f"{path}: sample {idx} stage durations exceed server_total_ms")

    communication = data.get("communication")
    if not isinstance(communication, dict):
        raise SweepError(f"{path}: missing communication object")
    required_communication = {
        "modeled_query_bytes_per_trial": (int, float),
        "actual_response_bytes_per_trial": (int, float),
        "modeled_helper_key_bytes": (int, float),
        "modeled_compression_key_bytes": (int, float),
        "query_bytes_kind": "modeled",
        "response_bytes_kind": "actual",
        "helper_key_bytes_kind": "modeled",
        "compression_key_bytes_kind": "modeled",
    }
    for key, expected in required_communication.items():
        if key not in communication:
            raise SweepError(f"{path}: missing communication.{key}")
        if isinstance(expected, str) and communication[key] != expected:
            raise SweepError(f"{path}: communication.{key} {communication[key]!r} != expected {expected!r}")
    for key, value in communication.items():
        if not key.endswith("_kind") and value is not None:
            if not isinstance(value, (int, float)) or isinstance(value, bool) or not math.isfinite(float(value)) or float(value) < 0:
                raise SweepError(f"{path}: communication.{key} is not a finite non-negative value")
    try:
        validate_sampled(data, path, experiments, leaf_count)
    except ProfileError as exc:
        raise SweepError(str(exc)) from exc

    return {
        "mean_server_total_ms": fmean(server_times),
        "samples": len(samples),
        "sample_leaves": leaves,
        "metadata": metadata,
        "communication": communication,
    }


def build_cases(output_dir: Path, g_values: list[int], n0_values: list[int]) -> list[Case]:
    cases: list[Case] = []
    for g in g_values:
        for n0 in n0_values:
            label = f"g{g}-n0{n0}"
            cases.append(
                Case(
                    label=label,
                    g=g,
                    n0=n0,
                    raw_json=output_dir / f"{label}.json",
                    process_json=output_dir / f"{label}-process.json",
                    log=output_dir / f"{label}.log",
                )
            )
    return cases


def command_for(binary: Path, case: Case, args: argparse.Namespace) -> list[str]:
    return [
        str(binary),
        "--test",
        "tree_multiplane_bench",
        "--leaf-count",
        str(args.leaf_count),
        "--experiments",
        str(args.experiments),
        "--warmup",
        str(args.warmup),
        "--trial-seed",
        str(args.trial_seed),
        "--benchmark-json",
        str(case.raw_json),
    ]


def environment_for(case: Case, args: argparse.Namespace) -> dict[str, str]:
    env = os.environ.copy()
    env.update(
        {
            "TREE_MULTIPLANE_G": str(case.g),
            "TREE_MULTIPLANE_N0": str(case.n0),
            "TREE_MULTIPLANE_ENTRY_BITS": str(args.entry_bits),
            "TREE_MULTIPLANE_PACKING": args.packing,
            "TREE_MULTIPLANE_QUERY_LAYOUT": args.query_layout,
        }
    )
    return env


def write_json(path: Path, data: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")


def existing_outputs(cases: list[Case], manifest_path: Path, ranking_path: Path) -> list[Path]:
    paths = [manifest_path, ranking_path]
    for case in cases:
        paths.extend([case.raw_json, case.process_json, case.log])
    return [path for path in paths if path.exists()]


def print_plan(cases: list[Case], binary: Path, args: argparse.Namespace) -> None:
    plan = []
    for case in cases:
        plan.append(
            {
                "label": case.label,
                "command": command_for(binary, case, args),
                "environment": {
                    key: environment_for(case, args)[key]
                    for key in (
                        "TREE_MULTIPLANE_G",
                        "TREE_MULTIPLANE_N0",
                        "TREE_MULTIPLANE_ENTRY_BITS",
                        "TREE_MULTIPLANE_PACKING",
                        "TREE_MULTIPLANE_QUERY_LAYOUT",
                    )
                },
                "output": str(case.raw_json),
                "process": str(case.process_json),
                "log": str(case.log),
            }
        )
    print(json.dumps({"cases": plan}, indent=2, sort_keys=True))


def run_sweep(args: argparse.Namespace) -> int:
    binary = args.binary.resolve()
    output_dir = args.output_dir.resolve()
    g, planes = select_layout(args.entry_bits, args.chunk_bits, args.ring_degree)
    cases = build_cases(output_dir, [g], args.n0_values)
    manifest_path = output_dir / "manifest.json"
    ranking_path = output_dir / "ranking.json"

    if args.plan_only:
        print_plan(cases, binary, args)
        return 0

    collisions = existing_outputs(cases, manifest_path, ranking_path)
    if collisions:
        raise SweepError(
            "output directory contains existing case data; choose a fresh directory: "
            + ", ".join(str(path) for path in collisions[:5])
        )

    output_dir.mkdir(parents=True, exist_ok=True)
    manifest: dict[str, Any] = {
        "schema_version": "tree_multiplane_sweep.v1",
        "state": "running",
        "binary": str(binary),
        "leaf_count": args.leaf_count,
        "entry_bits": args.entry_bits,
        "layout_policy": "maximum utilization, then minimum P; P<=6",
        "g": g,
        "plane_count": planes,
        "experiments": args.experiments,
        "warmup": args.warmup,
        "trial_seed": args.trial_seed,
        "response_packing": args.packing,
        "query_layout": args.query_layout,
        "cases": [],
    }
    write_json(manifest_path, manifest)

    completed = []
    failures = 0
    reference_leaves: tuple[int, ...] | None = None
    for case in cases:
        row: dict[str, Any] = {
            "label": case.label,
            "g": case.g,
            "N0": case.n0,
            "state": "running",
            "command": command_for(binary, case, args),
            "environment": {
                key: value
                for key, value in environment_for(case, args).items()
                if key.startswith("TREE_MULTIPLANE_")
            },
            "raw_json": str(case.raw_json),
            "process_json": str(case.process_json),
            "log": str(case.log),
        }
        manifest["cases"].append(row)
        write_json(manifest_path, manifest)
        process = run_measured(row["command"], log_path=case.log, env=environment_for(case, args))
        write_json(case.process_json, process)
        row["process"] = process
        if process.get("exit_code") != 0:
            row["state"] = "failed"
            row["error"] = "benchmark process returned nonzero exit"
            failures += 1
            write_json(manifest_path, manifest)
            continue
        try:
            summary = validate_result(
                case.raw_json,
                case=case,
                leaf_count=args.leaf_count,
                entry_bits=args.entry_bits,
                experiments=args.experiments,
                warmup=args.warmup,
                packing=args.packing,
                query_layout=args.query_layout,
                trial_seed=args.trial_seed,
            )
            if (int(summary["metadata"]["chunk_bits"]) != args.chunk_bits or
                    int(summary["metadata"]["ring_degree"]) != args.ring_degree):
                raise SweepError("binary scheme does not match --chunk-bits/--ring-degree")
            sample_leaves = tuple(summary["sample_leaves"])
            if reference_leaves is None:
                reference_leaves = sample_leaves
            elif sample_leaves != reference_leaves:
                raise SweepError("sample leaf schedule differs from first accepted case")
        except SweepError as exc:
            row["state"] = "failed"
            row["error"] = str(exc)
            failures += 1
            write_json(manifest_path, manifest)
            continue
        row["state"] = "complete"
        row.update(summary)
        completed.append(row)
        write_json(manifest_path, manifest)

    ranking = sorted(
        (
            {
                "label": row["label"],
                "g": row["g"],
                "N0": row["N0"],
                "mean_server_total_ms": row["mean_server_total_ms"],
                "samples": row["samples"],
                "sample_leaves": row["sample_leaves"],
                "metadata": row["metadata"],
                "communication": row["communication"],
                "raw_json": row["raw_json"],
                "process_json": row["process_json"],
            }
            for row in completed
        ),
        key=lambda row: (row["mean_server_total_ms"], row["g"], row["N0"]),
    )
    result = {
        "schema_version": "tree_multiplane_ranking.v1",
        "state": "complete" if completed else "failed",
        "rank_by": "mean complete server_total_ms",
        "completed": ranking,
        "failed": [row for row in manifest["cases"] if row["state"] == "failed"],
    }
    write_json(ranking_path, result)
    manifest["state"] = "complete_with_failures" if failures and completed else ("complete" if completed else "failed")
    write_json(manifest_path, manifest)
    if ranking:
        print(json.dumps(ranking[0], sort_keys=True))
    return 0 if completed else 1


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path, required=True)
    parser.add_argument("--leaf-count", type=int, required=True)
    parser.add_argument("--entry-bits", type=int, default=256)
    parser.add_argument("--chunk-bits", type=int, default=12)
    parser.add_argument("--ring-degree", type=int, default=2048)
    parser.add_argument("--n0-values", required=True)
    parser.add_argument("--experiments", type=int, default=3)
    parser.add_argument("--warmup", type=int, default=3)
    parser.add_argument("--trial-seed", type=int, default=7)
    parser.add_argument("--packing", choices=("mlwe", "projection"), default="mlwe")
    parser.add_argument("--query-layout", choices=("interleaved", "split-columns"), default="interleaved")
    parser.add_argument("--plan-only", action="store_true")
    args = parser.parse_args(argv)
    if args.leaf_count <= 0:
        parser.error("--leaf-count must be positive")
    if args.entry_bits <= 0:
        parser.error("--entry-bits must be positive")
    if args.experiments <= 0:
        parser.error("--experiments must be positive")
    if args.warmup < 0:
        parser.error("--warmup must be non-negative")
    select_layout(args.entry_bits, args.chunk_bits, args.ring_degree)
    args.n0_values = parse_power2_csv(args.n0_values, "--n0-values")
    return args


def main(argv: list[str] | None = None) -> int:
    try:
        return run_sweep(parse_args(argv))
    except SweepError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
