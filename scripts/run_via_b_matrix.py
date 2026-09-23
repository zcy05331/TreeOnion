#!/usr/bin/env python3
"""Run the VIA-B baseline: compiler pilot, per-height row-count pilots, formal rows.

--batch-mode native (the default) is the paper's VIA-B (Figure 9): the H
sibling queries are independent VIA-C queries over the whole database with the
answers repacked. --batch-mode pbc runs the same answer path per bucket of the
OnionPIR cuckoo baseline's PBC instance and is reported as the separate
via_b_pbc variant. The VIA authors build with clang; the system GCC build and
the LLVM-based icpx build are piloted at one height and the faster one serves
every height. The DMux row count I = 2^log_rows trades DMux nodes over q1
against CMux nodes over q2; the binary's default and its two neighbours are
piloted per height.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

from read_process_peak_memory import run_measured

SCALE_BY_HEIGHT = {22: "256mb", 23: "512mb", 24: "1gb", 25: "2gb", 26: "4gb", 27: "8gb"}


class ViaMatrixError(Exception):
    pass


SCHEME_BY_MODE = {"native": "via_b", "pbc": "via_b_pbc"}


def validate(path: Path, *, mode: str, height: int, trials: int, warmups: int, seed: int, log_rows) -> dict:
    try:
        data = json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as exc:
        raise ViaMatrixError(f"{path}: cannot load result: {exc}") from exc
    if data.get("scheme") != SCHEME_BY_MODE[mode] or data.get("height") != height:
        raise ViaMatrixError(f"{path}: scheme or height mismatch")
    if data.get("batch", {}).get("mode") != mode:
        raise ViaMatrixError(f"{path}: batch mode mismatch")
    if (data.get("measured_trials"), data.get("warmups"), data.get("trial_seed")) != (trials, warmups, seed):
        raise ViaMatrixError(f"{path}: trial counts or seed mismatch")
    rows = data.get("via_parameters", {}).get("log_rows")
    if not isinstance(rows, int) or (log_rows is not None and rows != log_rows):
        raise ViaMatrixError(f"{path}: log_rows mismatch")
    samples = data.get("samples")
    if not isinstance(samples, list) or len(samples) != trials or not all(s.get("correct") is True for s in samples):
        raise ViaMatrixError(f"{path}: expected {trials} correct measured samples")
    summary = data.get("measured_summary", {})
    if summary.get("all_correct") is not True or not summary.get("server_ms_mean", 0) > 0:
        raise ViaMatrixError(f"{path}: summary is not a correct positive timing")
    return {"server_ms_mean": summary["server_ms_mean"], "log_rows": rows, "raw": str(path)}


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--binary", action="append", required=True, metavar="LABEL=PATH",
                        help="adapter build to pilot, e.g. gcc=/path/via_b (repeatable)")
    parser.add_argument("--batch-mode", choices=sorted(SCHEME_BY_MODE), default="native")
    parser.add_argument("--heights", nargs="+", type=int, default=[22, 23, 24, 25, 26])
    parser.add_argument("--compiler-pilot-height", type=int, default=24)
    parser.add_argument("--warmups", type=int, default=3)
    parser.add_argument("--trials", type=int, default=8)
    parser.add_argument("--pilot-warmups", type=int, default=1)
    parser.add_argument("--pilot-trials", type=int, default=2)
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
    if args.trials <= 0 or args.pilot_trials <= 0 or args.warmups < 0 or args.pilot_warmups < 0:
        parser.error("trial counts must be positive")
    return args


def main(argv=None, *, runner=run_measured, gate=subprocess.run) -> int:
    args = parse_args(argv)
    root = args.root.resolve()
    tag = "via-b" if args.batch_mode == "native" else "via-b-pbc"
    scheme = SCHEME_BY_MODE[args.batch_mode]
    progress_path = root / f"metadata/{tag}-progress.json"
    progress = {"state": "running", "batch_mode": args.batch_mode,
                "binaries": {k: str(v) for k, v in args.binaries.items()},
                "path_semantics": "legacy_merkle_paths_omit_root", "compiler_pilot": {}, "heights": {}}

    def save():
        progress_path.parent.mkdir(parents=True, exist_ok=True)
        progress_path.write_text(json.dumps(progress, indent=2) + "\n")

    def run_one(binary, label, height, log_rows, warmups, trials, output):
        output.parent.mkdir(parents=True, exist_ok=True)
        command = ["numactl", "--physcpubind=0", "--preferred=0", str(binary), "--height", str(height),
                   "--warmup", str(warmups), "--trials", str(trials), "--trial-seed", str(args.trial_seed),
                   "--batch", args.batch_mode, "--compiler-label", label, "--output", str(output)]
        if log_rows is not None:
            command += ["--log-rows", str(log_rows)]
        gate([sys.executable, str(root / "wait_idle.py"), "--root", str(root)], check=True)
        process = runner(command, log_path=output.with_suffix(".log"), env=dict(os.environ, OMP_NUM_THREADS="1"))
        output.with_name(output.stem + "-process.json").write_text(json.dumps(process, indent=2) + "\n")
        if process.get("exit_code") != 0:
            raise ViaMatrixError(f"{output.name}: process exit {process.get('exit_code')}")
        return {**validate(output, mode=args.batch_mode, height=height, trials=trials, warmups=warmups,
                           seed=args.trial_seed, log_rows=log_rows),
                "command": command, "process": process}

    def attempt(store, key, *call):
        try:
            store[key] = run_one(*call)
        except ViaMatrixError as exc:
            store[key] = {"state": "failed", "error": str(exc)}
        save()
        return store[key]

    save()
    ph = args.compiler_pilot_height
    for label, binary in args.binaries.items():
        attempt(progress["compiler_pilot"], label, binary, label, ph, None, args.pilot_warmups, args.pilot_trials,
                root / f"outputs/{tag}/compiler-pilot" / f"{label}.json")
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
        out_dir = root / f"outputs/{tag}" / scale
        first = attempt(record["pilots"], "default", binary, chosen, height, None, args.pilot_warmups,
                        args.pilot_trials, out_dir / "pilot-rows-default.json")
        if "log_rows" in first:
            base = first["log_rows"]
            for rows in (base - 1, base + 1):
                if rows >= 0:
                    attempt(record["pilots"], str(rows), binary, chosen, height, rows, args.pilot_warmups,
                            args.pilot_trials, out_dir / f"pilot-rows-{rows}.json")
        good = [v for v in record["pilots"].values() if "server_ms_mean" in v]
        if not good:
            record.update(state="failed", error="no row-count pilot completed")
            save()
            continue
        best = min(good, key=lambda v: v["server_ms_mean"])["log_rows"]
        record["chosen_log_rows"] = best
        try:
            record["formal"] = run_one(binary, chosen, height, best, args.warmups, args.trials,
                                       out_dir / f"{scheme}.json")
            record["state"] = "complete"
        except ViaMatrixError as exc:
            record.update(state="failed", error=str(exc))
        save()
    ok = all(r["state"] == "complete" for r in progress["heights"].values())
    progress["state"] = "complete" if ok else "complete_with_failures"
    save()
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
