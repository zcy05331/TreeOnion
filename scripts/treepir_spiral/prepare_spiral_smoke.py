#!/usr/bin/env python3
"""Stage a minimal TreePIR Spiral Seperated smoke workspace.

This does not build Spiral and does not synthesize benchmark timings.  It only
checks one real color-bucket hash database plus local Spiral query offsets,
mirrors Spiral's hard-coded /tmp/Spiral directory layout, and emits
reproducible CPU build/run commands for the upstream Seperated target.
"""
from __future__ import annotations

import argparse
import json
import os
import re
import shutil
from pathlib import Path
from typing import Any

REPO_ROOT = Path(__file__).resolve().parents[2]
DEFAULT_SPIRAL_ROOT = REPO_ROOT / ".benchmarks" / "TreePIR" / "Spiral"

PRESETS: dict[int, dict[str, Any]] = {
    24: {
        "build": "TEXP=8 TEXPRIGHT=56 TCONV=4 TGSW=10 QPBITS=20 PVALUE=256 QNUMFIRST=1 QNUMREST=0 OUTN=2 ADDTL_CXX_FLAGS=",
        "run": ["9", "7", "46693", "a"],
    },
    26: {
        "build": "TEXP=8 TEXPRIGHT=56 TCONV=4 TGSW=11 QPBITS=21 PVALUE=256 QNUMFIRST=1 QNUMREST=0 OUTN=2 ADDTL_CXX_FLAGS=",
        "run": ["9", "9", "175209", "a"],
    },
    27: {
        "build": "TEXP=16 TEXPRIGHT=56 TCONV=4 TGSW=11 QPBITS=20 PVALUE=256 QNUMFIRST=1 QNUMREST=0 OUTN=2 ADDTL_CXX_FLAGS=",
        "run": ["9", "10", "354461", "a"],
    },
}

HEX64 = re.compile(r"^[0-9a-fA-F]{64}$")


def load_hash_count(path: Path) -> int:
    with path.open() as fh:
        payload = json.load(fh)
    if isinstance(payload, list) and len(payload) == 1 and isinstance(payload[0], dict):
        rows = payload[0]
    elif isinstance(payload, dict):
        rows = payload
    else:
        raise ValueError(f"{path} must be a Spiral color-bucket JSON map or one-element list containing a map")
    bad_keys = [k for k in rows.keys() if not str(k).isdigit()]
    if bad_keys:
        raise ValueError(f"{path} has non-integer hash indices, first bad key: {bad_keys[0]!r}")
    bad_hashes = [v for v in rows.values() if not isinstance(v, str) or not HEX64.match(v)]
    if bad_hashes:
        raise ValueError(f"{path} contains non-64-hex hash values")
    return len(rows)


def query_sets_for_height(payload: Any, height: int) -> list[list[int]]:
    if isinstance(payload, dict):
        value = payload.get(str(height), payload.get(height))
        if value is None and isinstance(payload.get("paths"), dict):
            value = payload["paths"].get(str(height), payload["paths"].get(height))
    else:
        value = payload
    if not isinstance(value, list) or not value:
        raise ValueError(f"indices JSON must contain a non-empty list for height {height}")
    if all(isinstance(index, int) for index in value):
        return [value]
    query_sets: list[list[int]] = []
    for query_set in value:
        if not isinstance(query_set, list) or not query_set:
            raise ValueError("each query set must be a non-empty list of integer local offsets")
        if not all(isinstance(index, int) for index in query_set):
            raise ValueError("each query set must contain only integer local offsets")
        query_sets.append(query_set)
    return query_sets


def stage(args: argparse.Namespace) -> dict[str, Any]:
    preset = PRESETS[args.height]
    database_path = args.database_json.resolve()
    index_bound = load_hash_count(database_path)

    with args.indices_json.open() as fh:
        all_query_sets = query_sets_for_height(json.load(fh), args.height)
    if args.path_id < 0 or args.path_id >= len(all_query_sets):
        raise ValueError(f"--path-id {args.path_id} outside available query sets 0..{len(all_query_sets)-1}")
    query_indices = all_query_sets[args.path_id]
    if any(index < 0 or index >= index_bound for index in query_indices):
        raise ValueError(f"query offsets contain an index outside color-bucket sequence range 0..{index_bound-1}")

    work_dir = args.work_dir.resolve()
    data_dir = work_dir / "Database" / "Colouring" / "Data"
    query_dir = work_dir / "Query_Storage"
    eval_dir = work_dir / "Evaluation" / "Data"
    for directory in (data_dir, query_dir, eval_dir):
        directory.mkdir(parents=True, exist_ok=True)

    database_name = args.database_name or database_path.stem
    staged_db = data_dir / database_name
    if staged_db.exists() or staged_db.is_symlink():
        staged_db.unlink()
    if args.copy_db:
        shutil.copy2(database_path, staged_db)
    else:
        os.symlink(database_path, staged_db)

    for query_file in query_dir.glob("*.query"):
        query_file.unlink()
    query_file = query_dir / f"h{args.height}_set{args.path_id}.query"
    query_file.write_text("\n".join(str(index) for index in query_indices) + "\n")

    build_script = (Path(__file__).resolve().parent / "build_spiral_cpu.sh").resolve()
    build_dir = args.build_dir.resolve() if args.build_dir else Path(f"/tmp/spiral-seperated-h{args.height}")
    executable = args.executable.resolve() if args.executable else build_dir / "Seperated" / "Seperated"
    run_args = [preset["run"][0], preset["run"][1], database_name, "a"]

    manifest = {
        "height": args.height,
        "hash_count": index_bound,
        "database_name": database_name,
        "query_set_id": args.path_id,
        "query_count": len(query_indices),
        "query_indices": query_indices,
        "query_index_basis": "local sequence offsets in this one color-bucket JSON, not JSON object keys",
        "spiral_root": str(args.spiral_root.resolve()),
        "work_dir": str(work_dir),
        "staged_database": str(staged_db),
        "query_file": str(query_file),
        "preset_build_variables": preset["build"],
        "upstream_reference_run": "./spiral " + " ".join(preset["run"]),
        "build_command": [str(build_script), "--height", str(args.height), "--spiral-root", str(args.spiral_root.resolve()), "--build-dir", str(build_dir)],
        "run_command": [str(executable), *run_args],
        "notes": [
            "Seperated loads hash JSON from /tmp/Spiral/Database/Colouring/Data/<argv3>; this wrapper stages one selected color bucket under --database-name.",
            "Query_Storage contains local offsets for warmup/measured leaf paths in that color bucket; Spiral iterates every .query entry after database generation.",
            "The fourth argv value is kept as 'a' to match upstream presets while leaving has_file disabled in this fork.",
        ],
    }
    if args.manifest:
        args.manifest.parent.mkdir(parents=True, exist_ok=True)
        args.manifest.write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n")
    return manifest


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--height", type=int, choices=sorted(PRESETS), required=True)
    parser.add_argument("--database-json", type=Path, required=True, help="Spiral color-bucket JSON for this CSA bucket")
    parser.add_argument("--indices-json", type=Path, required=True, help="JSON containing local query offsets keyed by 24, 26, or 27, or a direct list of offsets")
    parser.add_argument("--path-id", type=int, default=0, help="select a query set when the indices JSON contains a list of lists")
    parser.add_argument("--database-name", help="filename Spiral should load from /tmp/Spiral/Database/Colouring/Data; defaults to database JSON stem")
    parser.add_argument("--spiral-root", type=Path, default=DEFAULT_SPIRAL_ROOT)
    parser.add_argument("--work-dir", type=Path, default=Path("/tmp/Spiral"))
    parser.add_argument("--build-dir", type=Path)
    parser.add_argument("--executable", type=Path)
    parser.add_argument("--manifest", type=Path)
    parser.add_argument("--copy-db", action="store_true", help="copy database JSON instead of symlinking it")
    args = parser.parse_args()

    if not (args.spiral_root / "Seperated" / "src" / "spiral.cpp").exists():
        raise SystemExit(f"Spiral Seperated source not found under {args.spiral_root}")
    manifest = stage(args)
    print(json.dumps(manifest, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
