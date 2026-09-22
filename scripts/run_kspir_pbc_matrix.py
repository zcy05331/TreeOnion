#!/usr/bin/env python3
"""Run the KsPIR+PBC baseline: compiler pilot, per-height BSGS N1 pilots, formal rows.

The adapter reuses the OnionPIR cuckoo baseline's PBC instance bit for bit, so its
rows differ from the OnionPIR cuckoo rows only in the per-bucket PIR backend.
KsPIR's authors recommend clang; both the system GCC build and the LLVM-based
icpx build are piloted at one height and the faster one serves every height.
N1 (the BSGS baby-step count) trades per-query baby steps against per-instance
giant steps, so it is piloted per height.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

from read_process_peak_memory import run_measured

SCALE_BY_HEIGHT = {22: "256mb", 23: "512mb", 24: "1gb", 25: "2gb", 26: "4gb"}


class KspirMatrixError(Exception):
    pass


def validate(path: Path, *, height: int, trials: int, warmups: int, seed: int, n1: int) -> dict:
    try:
        data = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as exc:
        raise KspirMatrixError(f"{path}: cannot load result: {exc}") from exc
    if data.get("scheme") != "kspir_pbc" or data.get("height") != height:
        raise KspirMatrixError(f"{path}: scheme or height mismatch")
    if (data.get("measured_trials"), data.get("warmups"), data.get("trial_seed")) != (trials, warmups, seed):
        raise KspirMatrixError(f"{path}: trial counts or seed mismatch")
    if data.get("kspir_parameters", {}).get("bsgs_n1") != n1:
        raise KspirMatrixError(f"{path}: N1 mismatch")
    samples = data.get("samples")
    if not isinstance(samples, list) or len(samples) != trials or not all(s.get("correct") is True for s in samples):
        raise KspirMatrixError(f"{path}: expected {trials} correct measured samples")
    summary = data.get("measured_summary", {})
    if summary.get("all_correct") is not True or not summary.get("server_ms_mean", 0) > 0:
        raise KspirMatrixError(f"{path}: summary is not a correct positive timing")
    return {"server_ms_mean": summary["server_ms_mean"], "raw": str(path)}


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--binary", action="append", required=True, metavar="LABEL=PATH",
                        help="adapter build to pilot, e.g. gcc=/path/kspir_pbc (repeatable)")
    parser.add_argument("--heights", nargs="+", type=int, default=[22, 23, 24, 25, 26])
    parser.add_argument("--compiler-pilot-height", type=int, default=24)
    parser.add_argument("--n1-candidates", nargs="+", type=int, default=[64, 128, 256])
    parser.add_argument("--warmups", type=int, default=3)
    parser.add_argument("--trials", type=int, default=8)
    parser.add_argument("--pilot-trials", type=int, default=3)
    parser.add_argument("--trial-seed", type=int, default=7)
    args = parser.parse_args(argv)
    binaries = {}
    for item in args.binary:
        label, sep, path = item.partition("=")
        if not sep or not label or not path:
            parser.error(f"--binary expects LABEL=PATH, got {item!r}")
        binaries[label] = Path(path)
    args.binaries = binaries
    for height in [*args.heights, args.compiler_pilot_height]:
        if height not in SCALE_BY_HEIGHT:
            parser.error(f"unsupported height {height}")
    if any(n1 <= 1 or n1 & (n1 - 1) or 2048 % n1 for n1 in args.n1_candidates):
        parser.error("--n1-candidates must be powers of two dividing 2048")
    if args.trials <= 0 or args.pilot_trials <= 0 or args.warmups < 0:
        parser.error("trial counts must be positive")
    return args


def main(argv=None, *, runner=run_measured, gate=subprocess.run) -> int:
    args = parse_args(argv)
    root = args.root.resolve()
    progress_path = root / "metadata/kspir-pbc-progress.json"
    progress = {"state": "running", "binaries": {k: str(v) for k, v in args.binaries.items()},
                "path_semantics": "legacy_merkle_paths_omit_root", "compiler_pilot": {}, "heights": {}}

    def save():
        progress_path.parent.mkdir(parents=True, exist_ok=True)
        progress_path.write_text(json.dumps(progress, indent=2) + "\n")

    def run_one(binary, label, height, n1, trials, output):
        output.parent.mkdir(parents=True, exist_ok=True)
        command = ["numactl", "--physcpubind=0", "--preferred=0", str(binary), "--height", str(height),
                   "--warmup", str(args.warmups), "--trials", str(trials), "--trial-seed", str(args.trial_seed),
                   "--n1", str(n1), "--compiler-label", label, "--output", str(output)]
        gate([sys.executable, str(root / "wait_idle.py"), "--root", str(root)], check=True)
        process = runner(command, log_path=output.with_suffix(".log"), env=dict(os.environ, OMP_NUM_THREADS="1"))
        output.with_name(output.stem + "-process.json").write_text(json.dumps(process, indent=2) + "\n")
        if process.get("exit_code") != 0:
            raise KspirMatrixError(f"{output.name}: process exit {process.get('exit_code')}")
        return {**validate(output, height=height, trials=trials, warmups=args.warmups, seed=args.trial_seed, n1=n1),
                "command": command, "process": process}

    save()
    # Compiler pilot at one height with the upstream default N1 = 128.
    ph = args.compiler_pilot_height
    for label, binary in args.binaries.items():
        out = root / "outputs/kspir-pbc/compiler-pilot" / f"{label}.json"
        try:
            progress["compiler_pilot"][label] = run_one(binary, label, ph, 128, args.pilot_trials, out)
        except KspirMatrixError as exc:
            progress["compiler_pilot"][label] = {"state": "failed", "error": str(exc)}
        save()
    ok_labels = [l for l, r in progress["compiler_pilot"].items() if "server_ms_mean" in r]
    if not ok_labels:
        progress["state"] = "failed"
        save()
        return 1
    chosen = min(ok_labels, key=lambda l: progress["compiler_pilot"][l]["server_ms_mean"])
    progress["chosen_compiler"] = chosen
    save()

    binary = args.binaries[chosen]
    for height in args.heights:
        scale = SCALE_BY_HEIGHT[height]
        record = {"state": "running", "pilots": {}}
        progress["heights"][str(height)] = record
        save()
        for n1 in args.n1_candidates:
            out = root / "outputs/kspir-pbc" / scale / f"pilot-n1-{n1}.json"
            try:
                record["pilots"][str(n1)] = run_one(binary, chosen, height, n1, args.pilot_trials, out)
            except KspirMatrixError as exc:
                record["pilots"][str(n1)] = {"state": "failed", "error": str(exc)}
            save()
        good = {int(k): v for k, v in record["pilots"].items() if "server_ms_mean" in v}
        if not good:
            record.update(state="failed", error="no N1 pilot completed")
            save()
            continue
        best_n1 = min(good, key=lambda k: good[k]["server_ms_mean"])
        record["chosen_n1"] = best_n1
        try:
            record["formal"] = run_one(binary, chosen, height, best_n1, args.trials,
                                       root / "outputs/kspir-pbc" / scale / "kspir_pbc.json")
            record["state"] = "complete"
        except KspirMatrixError as exc:
            record.update(state="failed", error=str(exc))
        save()
    ok = all(r["state"] == "complete" for r in progress["heights"].values())
    progress["state"] = "complete" if ok else "complete_with_failures"
    save()
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
