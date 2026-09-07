#!/usr/bin/env python3
"""Run the Generic entry-size sweep: 1 GiB, except 48B at the 32B leaf count."""

from __future__ import annotations

import argparse
import datetime as _dt
import json
import math
import os
import pathlib
import shutil
import subprocess
import sys
from typing import Any, Callable

from analyze_superrecord_sweep import analyze_run
from read_process_peak_memory import run_measured
from summarize_cloudlab_profiles import ProfileError, validate_profile

ROOT = pathlib.Path(__file__).resolve().parents[1]
LOGICAL_DB_BYTES = 1 << 30
BASELINE_32B_LEAF_COUNT = 1 << 24
DEFAULT_ENTRY_BYTES = (16, 48, 64, 128)
TRIALS = 100
WARMUPS = 3
SEED = 7
BOOTSTRAP_SEED = 90304
EXECUTION_SEED = 20260904
MEMORY_GIB = 320


class EntrySweepError(RuntimeError):
    pass


def entry_plan(entry_bytes: int) -> dict[str, int]:
    if entry_bytes == 48:
        # Requested same tree as the 32B reference, hence a 1.5 GiB tier.
        leaf_count = BASELINE_32B_LEAF_COUNT
    elif entry_bytes <= 0 or (BASELINE_32B_LEAF_COUNT * 32) % entry_bytes != 0:
        raise EntrySweepError(f"entry size must preserve the 1GiB full-tree target: {entry_bytes}")
    else:
        leaf_count = (BASELINE_32B_LEAF_COUNT * 32) // entry_bytes
    if leaf_count & (leaf_count - 1):
        raise EntrySweepError(f"leaf count must be a power of two: {leaf_count}")
    return {
        "entry_bytes": entry_bytes,
        "entry_bits": entry_bytes * 8,
        "leaf_count": leaf_count,
        "tree_height": leaf_count.bit_length() - 1,
        "logical_db_bytes": 2 * leaf_count * entry_bytes,
        "actual_tree_bytes": (2 * leaf_count - 1) * entry_bytes,
        "max_responses": max(1, math.ceil(entry_bytes / 32)),
    }


def process_path_for(raw_path: pathlib.Path) -> pathlib.Path:
    return raw_path.with_name(raw_path.stem + "-process.json")


def log_path_for(raw_path: pathlib.Path) -> pathlib.Path:
    return raw_path.with_suffix(".log")


def load_json(path: pathlib.Path) -> dict[str, Any]:
    try:
        data = json.loads(path.read_text())
    except FileNotFoundError as exc:
        raise EntrySweepError(f"missing JSON: {path}") from exc
    except json.JSONDecodeError as exc:
        raise EntrySweepError(f"invalid JSON {path}: {exc}") from exc
    if not isinstance(data, dict):
        raise EntrySweepError(f"JSON root is not an object: {path}")
    return data


def validate_process(path: pathlib.Path) -> dict[str, Any]:
    process = load_json(path)
    if process.get("exit_code") != 0:
        raise EntrySweepError(f"{path}: process exit_code is not 0")
    return process


def validate_bundle_arities(data: dict[str, Any], raw_path: pathlib.Path) -> None:
    subset = data.get("benchmark_bundle_arities")
    if subset is not None and subset != [1, 3]:
        raise EntrySweepError(f"{raw_path}: benchmark_bundle_arities {subset} != [1, 3]")
    measured = []
    for candidate in data.get("candidates", []):
        samples = candidate.get("samples")
        if not candidate.get("eligible", True) or not isinstance(samples, list) or not samples:
            continue
        shape = candidate.get("shape") or {}
        measured.append(shape.get("bundle_arity"))
    unexpected = sorted({arity for arity in measured if arity not in (1, 3)})
    if unexpected:
        raise EntrySweepError(f"{raw_path}: measured bundle arities include {unexpected}, expected only 1 and 3")
    if 1 not in measured:
        raise EntrySweepError(f"{raw_path}: missing measured h1 candidate")
    if 3 not in measured:
        raise EntrySweepError(f"{raw_path}: missing measured h3 candidate")


def validate_generic_raw(raw_path: pathlib.Path, process_path: pathlib.Path, plan: dict[str, int]) -> dict[str, Any]:
    process = validate_process(process_path)
    data = load_json(raw_path)
    validate_bundle_arities(data, raw_path)
    try:
        validate_profile(data, raw_path, TRIALS, plan["leaf_count"])
    except ProfileError as exc:
        raise EntrySweepError(str(exc)) from exc
    if data.get("entry_bits") != plan["entry_bits"]:
        raise EntrySweepError(f"{raw_path}: entry_bits {data.get('entry_bits')} != {plan['entry_bits']}")
    if data.get("measured_trials") != TRIALS:
        raise EntrySweepError(f"{raw_path}: measured_trials {data.get('measured_trials')} != {TRIALS}")
    if data.get("warmup_count") != WARMUPS:
        raise EntrySweepError(f"{raw_path}: warmup_count {data.get('warmup_count')} != {WARMUPS}")
    return {"raw": data, "process": process}


def copy_if_present(source: pathlib.Path, dest: pathlib.Path) -> str | None:
    if not source.exists():
        return None
    if source.resolve() == dest.resolve():
        return str(dest)
    shutil.copy2(source, dest)
    return str(dest)


def command_for(binary: pathlib.Path, plan: dict[str, int]) -> list[str]:
    return [
        str(binary),
        "--test", "tree_superrecord_bench",
        "--leaf-count", str(plan["leaf_count"]),
        "--experiments", str(TRIALS),
        "--warmup", str(WARMUPS),
        "--trial-seed", str(SEED),
    ]


def env_for(plan: dict[str, int], raw_path: pathlib.Path, source_commit: str | None = None) -> dict[str, str]:
    env = os.environ.copy()
    env.setdefault("SUPERRECORD_MEMORY_BYTES", str(MEMORY_GIB * 1024 ** 3))
    if source_commit:
        env["SUPERRECORD_COMMIT"] = source_commit
    env.update({
        "SUPERRECORD_ENTRY_BITS": str(plan["entry_bits"]),
        "SUPERRECORD_OUTPUT_JSON": str(raw_path),
        "SUPERRECORD_SKIP_LEGACY": "1",
        "SUPERRECORD_MAX_RESPONSES": str(plan["max_responses"]),
        "SUPERRECORD_MAX_FRONTIER": "2",
        "SUPERRECORD_MAX_ARITY": "3",
        "SUPERRECORD_BUNDLE_ARITIES": "1,3",
        "SUPERRECORD_FIXED_KEY_HEIGHT": "11",
        "SUPERRECORD_MIN_NOISE_MARGIN": "0",
        "SUPERRECORD_KEY_MODE": "fixed",
        "SUPERRECORD_EXECUTION_SEED": str(EXECUTION_SEED),
        "SUPERRECORD_BOOTSTRAP_SEED": str(BOOTSTRAP_SEED),
    })
    return env


def best_candidates(raw: dict[str, Any], analysis: dict[str, Any]) -> tuple[dict[str, Any], dict[str, Any]]:
    rows = [row for row in analysis["candidates"] if not row.get("legacy")]
    h1 = [row for row in rows if (row.get("shape") or {}).get("bundle_arity") == 1]
    h3 = [row for row in rows if (row.get("shape") or {}).get("bundle_arity") == 3]
    if not h1:
        raise EntrySweepError("analysis has no measured h1 candidate")
    if not h3:
        raise EntrySweepError("analysis has no measured h3 candidate")
    raw_by_label = {row.get("label"): row for row in raw.get("candidates", [])}

    def attach(row: dict[str, Any]) -> dict[str, Any]:
        source = raw_by_label.get(row["label"])
        if not isinstance(source, dict):
            raise EntrySweepError(f"raw candidate missing for {row['label']}")
        merged = dict(row)
        merged["wire"] = source.get("wire") or {}
        merged["candidate_metrics"] = source.get("candidate_metrics") or {}
        return merged

    return attach(min(h1, key=lambda row: row["server_stats"]["mean"])), attach(min(h3, key=lambda row: row["server_stats"]["mean"]))



def mean_first_selection(raw: dict[str, Any], analysis: dict[str, Any]) -> dict[str, dict[str, Any]]:
    rows = [row for row in analysis["candidates"] if not row.get("legacy")]
    if not rows:
        raise EntrySweepError("analysis has no measured generic candidates")
    h1, h3 = best_candidates(raw, analysis)
    overall = min(rows, key=lambda row: (row["server_stats"]["mean"], row["label"]))

    def summarize(row: dict[str, Any]) -> dict[str, Any]:
        return {"label": row["label"], "mean_ms": row["server_stats"]["mean"]}

    return {
        "best_overall": summarize(overall),
        "best_h1": summarize(h1),
        "best_h3": summarize(h3),
    }

def render_entry_report(entry_dir: pathlib.Path, plan: dict[str, int], raw: dict[str, Any], analysis: dict[str, Any], *, reused_source: pathlib.Path | None) -> None:
    h1, h3 = best_candidates(raw, analysis)

    def row(label: str, candidate: dict[str, Any]) -> list[str]:
        stats = candidate["server_stats"]
        stages = candidate.get("stage_stats") or {}
        stage_bits = ", ".join(f"{name}={value['mean']:.3f}ms" for name, value in sorted(stages.items()))
        wire = candidate["wire"]
        metrics = candidate["candidate_metrics"]
        shape = candidate["shape"]
        layout = " ".join(f"{label}={shape.get(key, '—')}" for label, key in (
            ("h", "bundle_arity"), ("G", "record_coeffs"), ("P", "plane_count"),
            ("N0", "N0"), ("b", "beta_bits"), ("hq", "expansion_height")))
        return [
            label,
            candidate["label"],
            layout,
            f"{stats['mean']:.3f}",
            f"{plan['actual_tree_bytes'] / 2**20 / (stats['mean'] / 1000):.3f}",
            f"{stats['median']:.3f}",
            f"{stats['p95']:.3f}",
            f"{stats['stddev']:.3f}",
            str(wire.get("modeled_seeded_query_bytes", "—")),
            str(wire.get("actual_response_ciphertext_payload_bytes", "—")),
            str(metrics.get("modeled_per_client_eval_key_bytes", "—")),
            stage_bits,
        ]

    table_rows = [row("best h3", h3), row("best h1", h1)]
    header = ["category", "candidate", "layout", "mean ms", "logical DB MiB/s", "median ms", "p95 ms", "SD ms", "query modeled B", "response actual B", "once keys modeled B", "stage mean ms"]
    table = ["| " + " | ".join(header) + " |", "| " + " | ".join("---" for _ in header) + " |"]
    table += ["| " + " | ".join(items) + " |" for items in table_rows]
    source_note = ""
    if reused_source is not None:
        commit = str(raw.get("repository_commit") or "unknown")[:7]
        source_note = (
            f"\n\nReused 32B source: `{reused_source}` at source `{commit}`. "
            "Preprocessing time belongs to this original run; compare setup costs across entry sizes only when the implementation and execution conditions match."
        )
        if commit == "22adeee":
            source_note += " This reference predates the bytewise preprocessing optimization used by the new runs."
    lines = [
        f"# Generic Entry {plan['entry_bytes']}B Sweep",
        "",
        f"Entry bits: {plan['entry_bits']}. Leaves: {plan['leaf_count']} (2^{plan['tree_height']}). Trials: {TRIALS}; warmups: {WARMUPS}.",
        f"Logical DB bytes: {plan['logical_db_bytes']}. Actual full tree bytes: {plan['actual_tree_bytes']}.",
        "Logical DB throughput uses actual full-tree bytes divided by mean server time. It is not network throughput or padded-storage throughput.",
        "",
        *table,
        source_note,
        "",
    ]
    (entry_dir / "REPORT.md").write_text("\n".join(lines).replace("\n\n\n", "\n\n"))


def analyze_and_write(raw_path: pathlib.Path, analysis_path: pathlib.Path, report_path: pathlib.Path, plan: dict[str, int], *, reused_source: pathlib.Path | None) -> dict[str, Any]:
    raw = load_json(raw_path)
    analysis = analyze_run(raw, seed=BOOTSTRAP_SEED)
    analysis_path.write_text(json.dumps(analysis, indent=2, allow_nan=False) + "\n")
    render_entry_report(report_path.parent, plan, raw, analysis, reused_source=reused_source)
    return analysis


def run_entry(
    entry_dir: pathlib.Path,
    binary: pathlib.Path,
    plan: dict[str, int],
    *,
    source_commit: str | None = None,
    run_func: Callable[..., dict[str, Any]] = run_measured,
) -> dict[str, Any]:
    entry_dir.mkdir(parents=True, exist_ok=True)
    raw_path = entry_dir / "raw.json"
    process_path = process_path_for(raw_path)
    log_path = log_path_for(raw_path)
    analysis_path = entry_dir / "analysis.json"
    if any(path.exists() for path in (raw_path, process_path, log_path, analysis_path)):
        raise EntrySweepError(f"refusing to overwrite existing entry output in {entry_dir}")
    command = command_for(binary, plan)
    process = run_func(command, log_path=log_path, env=env_for(plan, raw_path, source_commit), cwd=ROOT)
    process_path.write_text(json.dumps(process, indent=2, allow_nan=False) + "\n")
    if process.get("exit_code") != 0:
        raise EntrySweepError(f"entry {plan['entry_bytes']}B benchmark failed; see {log_path}")
    validate_generic_raw(raw_path, process_path, plan)
    analysis = analyze_and_write(raw_path, analysis_path, entry_dir / "REPORT.md", plan, reused_source=None)
    return {"raw_file": str(raw_path), "process_file": str(process_path), "log_file": str(log_path), "analysis_file": str(analysis_path), "analysis": analysis}


def reuse_entry(entry_dir: pathlib.Path, source_raw: pathlib.Path, plan: dict[str, int]) -> dict[str, Any]:
    entry_dir.mkdir(parents=True, exist_ok=True)
    raw_dest = entry_dir / "raw.json"
    process_dest = process_path_for(raw_dest)
    log_dest = log_path_for(raw_dest)
    analysis_path = entry_dir / "analysis.json"
    if any(path.exists() for path in (raw_dest, process_dest, log_dest, analysis_path)):
        raise EntrySweepError(f"refusing to overwrite existing entry output in {entry_dir}")
    source_raw = source_raw.resolve()
    source_process = process_path_for(source_raw)
    validate_generic_raw(source_raw, source_process, plan)
    copy_if_present(source_raw, raw_dest)
    copy_if_present(source_process, process_dest)
    copied_log = copy_if_present(log_path_for(source_raw), log_dest)
    if copied_log is None:
        (entry_dir / "log-source.txt").write_text(str(log_path_for(source_raw)) + "\n")
    analysis = analyze_and_write(raw_dest, analysis_path, entry_dir / "REPORT.md", plan, reused_source=source_raw)
    return {"raw_file": str(raw_dest), "source_raw_file": str(source_raw), "process_file": str(process_dest), "log_file": str(log_dest) if copied_log else None, "analysis_file": str(analysis_path), "analysis": analysis}


def write_manifest(path: pathlib.Path, manifest: dict[str, Any]) -> None:
    path.write_text(json.dumps(manifest, indent=2, allow_nan=False) + "\n")


def build_manifest(output_dir: pathlib.Path, entries: list[int], binary: pathlib.Path | None, *, plan_only: bool, source_commit: str | None = None) -> dict[str, Any]:
    cases = []
    for entry_bytes in entries:
        plan = entry_plan(entry_bytes)
        entry_dir = output_dir / f"entry_{entry_bytes}B"
        raw_path = entry_dir / "raw.json"
        case = {
            **plan,
            "entry_dir": str(entry_dir),
            "raw_file": str(raw_path),
            "process_file": str(process_path_for(raw_path)),
            "log_file": str(log_path_for(raw_path)),
            "analysis_file": str(entry_dir / "analysis.json"),
            "report_file": str(entry_dir / "REPORT.md"),
            "trials": TRIALS,
            "warmups": WARMUPS,
            "command": command_for(binary or pathlib.Path("Onion-PIR"), plan),
            "environment": {k: v for k, v in env_for(plan, raw_path, source_commit).items() if k.startswith("SUPERRECORD_")},
            "state": "planned" if plan_only else "pending",
        }
        cases.append(case)
    return {
        "schema_version": "generic-entry-size-sweep.v1",
        "created_utc": _dt.datetime.now(_dt.timezone.utc).isoformat(),
        "reference_db_bytes": LOGICAL_DB_BYTES,
        "capacity_policy": "48B uses the 32B leaf count (1.5 GiB); other entries target 1 GiB",
        "default_entry_order_bytes": list(DEFAULT_ENTRY_BYTES),
        "binary_source_commit": source_commit or os.environ.get("SUPERRECORD_COMMIT"),
        "selection_policy": "mean_server_total_ms_primary; analyzer CI metadata is retained but does not override this entry-size selection",
        "acceptance_policy": "decryption_correctness_only; noise margins are diagnostic",
        "cases": cases,
        "state": "planned" if plan_only else "running",
    }


def main(argv: list[str] | None = None) -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--binary", type=pathlib.Path, help="Onion-PIR benchmark binary for new entry sizes")
    parser.add_argument("--output-dir", type=pathlib.Path, default=ROOT / "outputs/superrecord/entry-size-sweep-1gb")
    parser.add_argument("--entries", default=",".join(str(x) for x in DEFAULT_ENTRY_BYTES), help="comma-separated entry sizes in bytes")
    parser.add_argument("--reuse-32b", type=pathlib.Path, help="reuse an existing 32B/entry_bits=256 generic raw JSON")
    parser.add_argument("--plan-only", action="store_true", help="write a manifest without running crypto")
    parser.add_argument("--source-commit", help="compiled benchmark source commit; defaults to inherited SUPERRECORD_COMMIT")
    args = parser.parse_args(argv)

    entries = [int(part) for part in args.entries.split(",") if part]
    if not entries or len(set(entries)) != len(entries):
        parser.error("--entries must select at least one distinct entry size")
    output_dir = args.output_dir.resolve()
    output_dir.mkdir(parents=True, exist_ok=True)
    binary = args.binary.resolve() if args.binary else None
    reused_entries = {32} if args.reuse_32b is not None else set()
    entries_requiring_binary = [entry for entry in entries if entry not in reused_entries]
    if not args.plan_only and entries_requiring_binary and binary is None:
        parser.error("--binary is required for entries that are not supplied by --reuse-32b")
    if binary is not None and not binary.is_file():
        parser.error(f"benchmark binary does not exist: {binary}")

    source_commit = args.source_commit or os.environ.get("SUPERRECORD_COMMIT")
    manifest = build_manifest(output_dir, entries, binary, plan_only=args.plan_only, source_commit=source_commit)
    manifest_path = output_dir / "manifest.json"
    write_manifest(manifest_path, manifest)
    if args.plan_only:
        print(f"Plan: {manifest_path}")
        return 0

    failures = 0
    for case in manifest["cases"]:
        plan = {key: case[key] for key in ("entry_bytes", "entry_bits", "leaf_count", "tree_height", "logical_db_bytes", "actual_tree_bytes", "max_responses")}
        entry_dir = pathlib.Path(case["entry_dir"])
        case["state"] = "reusing" if plan["entry_bytes"] == 32 and args.reuse_32b is not None else "running"
        write_manifest(manifest_path, manifest)
        try:
            if plan["entry_bytes"] == 32 and args.reuse_32b is not None:
                result = reuse_entry(entry_dir, args.reuse_32b, plan)
                case["state"] = "reused"
            else:
                result = run_entry(entry_dir, binary, plan, source_commit=source_commit)
                case["state"] = "complete"
            case.update({k: v for k, v in result.items() if k != "analysis"})
            best = mean_first_selection(load_json(pathlib.Path(case["raw_file"])), result["analysis"])
            case["best_overall"] = best["best_overall"]
            case["best_h1"] = best["best_h1"]
            case["best_h3"] = best["best_h3"]
        except Exception as exc:
            failures += 1
            case["state"] = "failed"
            case["error"] = str(exc)
        write_manifest(manifest_path, manifest)
    manifest["state"] = "failed" if failures else "complete"
    write_manifest(manifest_path, manifest)
    print(f"Artifacts: {output_dir}")
    return 1 if failures else 0


if __name__ == "__main__":
    raise SystemExit(main())
