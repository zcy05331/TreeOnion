#!/usr/bin/env python3
"""Build/run wrapper for Respire and dry-run generator for the shared path workload."""
from __future__ import annotations

import argparse
import json
import os
import subprocess
import sys
from pathlib import Path


ROOT = Path(__file__).resolve().parent
PATH_BASELINES = ROOT.parent
sys.path.insert(0, str(PATH_BASELINES))
import workload  # noqa: E402
import respire.stage_respire as stage_respire  # noqa: E402

REPO_ROOT = ROOT.parents[2]
STAGE_ROOT = REPO_ROOT / ".benchmarks" / "respire-path"
STAGED_RESPIRE = STAGE_ROOT / "respire"
MANIFEST = STAGED_RESPIRE / "Cargo.toml"
SOURCE_COMMIT = "fd20d38e9e94c0298ab21af2f7699a23729eef67"
BATCH_SIZE = 32


def read_leaf_prefix(path: Path, count: int, height: int) -> list[int]:
    leaves: list[int] = []
    for line_no, raw in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        line = raw.split("#", 1)[0].strip()
        if not line:
            continue
        try:
            leaf = int(line)
        except ValueError as exc:
            raise SystemExit(f"{path}:{line_no}: invalid leaf") from exc
        leaves.append(leaf)
    if len(leaves) < count:
        raise SystemExit(f"leaf file has {len(leaves)} leaves; requested warmup+trials={count}")
    selected = leaves[:count]
    for leaf in selected:
        if leaf >= 1 << height:
            raise SystemExit(f"leaf {leaf} is outside height-{height} range")
    return selected


def request_indices(height: int, leaf: int) -> list[int]:
    path = workload.path_indices(height, leaf)
    used = set(path)
    indices = list(path)
    candidate = (1 << (height + 1)) - 1
    while len(indices) < BATCH_SIZE:
        if candidate not in used:
            used.add(candidate)
            indices.append(candidate)
        candidate -= 1
    return indices


def dry_run(args: argparse.Namespace) -> dict:
    leaves = read_leaf_prefix(args.leaf_file, args.warmup + args.trials, args.height)
    rows = []
    for i, leaf in enumerate(leaves):
        path = workload.path_indices(args.height, leaf)
        rows.append(
            {
                "iteration": i,
                "phase": "warmup" if i < args.warmup else "measured",
                "leaf": leaf,
                "path_indices": path,
                "request_indices": request_indices(args.height, leaf),
                "expected_path_hex": [workload.record_bytes(index).hex() for index in path],
            }
        )
    return {
        "schema_version": "path-baseline-v1",
        "scheme": "respire",
        "path_mode": workload.PATH_MODE,
        "height": args.height,
        "leaf_count": 1 << args.height,
        "record_bytes": workload.RECORD_BYTES,
        "logical_records": (1 << (args.height + 1)) - 1,
        "logical_db_bytes": ((1 << (args.height + 1)) - 1) * workload.RECORD_BYTES,
        "backend_records": 1 << (args.height + 1),
        "batch_size": BATCH_SIZE,
        "warmups": args.warmup,
        "trials": args.trials,
        "dry_run_samples": rows,
    }


def run_cargo(args: argparse.Namespace) -> int:
    binary = os.environ.get("RESPIRE_PATH_ADAPTER_BIN")
    if binary:
        command = [binary]
    else:
        ensure_staged(args.manifest_path)
        command = [
            "cargo",
            "run",
            "--locked",
            "--release",
            "--bin",
            "path_workload",
            "--manifest-path",
            str(args.manifest_path),
            "--",
        ]
    command.extend(
        [
            "--height",
            str(args.height),
            "--leaf-file",
            str(args.leaf_file),
            "--warmup",
            str(args.warmup),
            "--trials",
            str(args.trials),
            "--output",
            args.output,
        ]
    )
    env = os.environ.copy()
    env.setdefault("CARGO_BUILD_JOBS", "2")
    env.setdefault("CARGO_TARGET_DIR", str(STAGE_ROOT / "cargo-target"))
    return subprocess.run(command, cwd=ROOT, env=env).returncode


def ensure_staged(manifest_path: Path) -> None:
    source_override = os.environ.get("RESPIRE_SOURCE_DIR")
    stage_respire.stage(ROOT, STAGE_ROOT, Path(source_override) if source_override else None)
    if not manifest_path.exists():
        raise SystemExit(f"staged Respire manifest does not exist: {manifest_path}")


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--height", type=int, required=True)
    parser.add_argument("--leaf-file", type=Path, required=True)
    parser.add_argument("--warmup", type=int, required=True)
    parser.add_argument("--trials", type=int, required=True)
    parser.add_argument("--output", default="-")
    parser.add_argument("--manifest-path", type=Path, default=MANIFEST)
    parser.add_argument("--dry-run", action="store_true")
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    if args.trials <= 0:
        raise SystemExit("--trials must be greater than zero")
    if args.height not in (8, 22, 23, 24, 25, 26):
        raise SystemExit("supported heights are 8, 22, 23, 24, 25, and 26")
    if args.dry_run:
        text = json.dumps(dry_run(args), indent=2) + "\n"
        if args.output == "-":
            print(text, end="")
        else:
            Path(args.output).write_text(text, encoding="utf-8")
        return 0
    return run_cargo(args)


if __name__ == "__main__":
    raise SystemExit(main())
