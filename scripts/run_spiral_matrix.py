#!/usr/bin/env python3
"""Run a Spiral baseline per height with the shared-host idle policy.

--scheme spiral_pbc puts Spiral behind the OnionPIR cuckoo baseline's PBC
instance bit for bit (same records, hashes, buckets, positions and leaf
schedule), so its rows differ from the OnionPIR cuckoo rows only in the
per-bucket PIR backend. --scheme treepir_spiral puts it behind TreePIR's color
buckets: colors, color-local indices and leaves come from the TreePIR row's
formal.json under --treepir-root, so its rows differ from the TreePIR row only
in the per-color PIR backend.
"""
import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

from read_process_peak_memory import run_measured

SCALE_BY_HEIGHT = {22: "256mb", 23: "512mb", 24: "1gb", 25: "2gb", 26: "4gb", 27: "8gb"}


class SpiralMatrixError(Exception):
    pass


# scheme -> output and progress tag
SCHEMES = {"spiral_pbc": "spiral-pbc", "treepir_spiral": "treepir-spiral"}


def validate(result_path: Path, *, scheme: str, height: int, trials: int, warmups: int, seed: int) -> dict:
    try:
        data = json.loads(result_path.read_text())
    except (OSError, json.JSONDecodeError) as exc:
        raise SpiralMatrixError(f"{result_path}: cannot load result: {exc}") from exc
    if data.get("scheme") != scheme or data.get("height") != height:
        raise SpiralMatrixError(f"{result_path}: scheme or height mismatch")
    if (data.get("measured_trials"), data.get("warmups"), data.get("trial_seed")) != (trials, warmups, seed):
        raise SpiralMatrixError(f"{result_path}: trial counts or seed mismatch")
    samples = data.get("samples")
    if not isinstance(samples, list) or len(samples) != trials:
        raise SpiralMatrixError(f"{result_path}: expected {trials} measured samples")
    if not all(sample.get("correct") is True for sample in samples):
        raise SpiralMatrixError(f"{result_path}: a measured sample is not correct")
    summary = data.get("measured_summary", {})
    if summary.get("all_correct") is not True or not summary.get("server_ms_mean", 0) > 0:
        raise SpiralMatrixError(f"{result_path}: summary is not a correct positive timing")
    return {"server_ms_mean": summary["server_ms_mean"],
            "query_bytes_per_path": data["communication"]["query_bytes_per_path"],
            "response_bytes_per_path": data["communication"]["response_bytes_per_path"],
            "public_parameter_bytes": data["communication"]["public_parameter_bytes"]}


def parse_args(argv=None):
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--scheme", choices=sorted(SCHEMES), required=True)
    parser.add_argument("--binary", type=Path)
    parser.add_argument("--treepir-root", type=Path,
                        help="treepir-backend-tuning output directory holding <scale>/formal.json (treepir_spiral)")
    parser.add_argument("--heights", nargs="+", type=int, default=[22, 23, 24, 25, 26])
    parser.add_argument("--warmups", type=int, default=3)
    parser.add_argument("--trials", type=int, default=8)
    parser.add_argument("--trial-seed", type=int, default=7)
    args = parser.parse_args(argv)
    for height in args.heights:
        if height not in SCALE_BY_HEIGHT:
            parser.error(f"unsupported height {height}; expected one of {sorted(SCALE_BY_HEIGHT)}")
    if args.trials <= 0 or args.warmups < 0:
        parser.error("--trials must be positive and --warmups nonnegative")
    if args.scheme == "treepir_spiral" and args.treepir_root is None:
        parser.error("--scheme treepir_spiral needs --treepir-root")
    return args


def main(argv=None, *, runner=run_measured, gate=subprocess.run) -> int:
    args = parse_args(argv)
    root = args.root.resolve()
    scheme, tag = args.scheme, SCHEMES[args.scheme]
    binary = (args.binary or root / f"source/.benchmarks/spiral-pbc/target/release/{scheme}").resolve()
    progress_path = root / f"metadata/{tag}-progress.json"
    progress = {"state": "running", "scheme": scheme, "binary": str(binary), "heights": {},
                "path_semantics": "legacy_merkle_paths_omit_root" if scheme == "spiral_pbc"
                else "legacy_colored_path_omits_root"}

    def save():
        progress_path.parent.mkdir(parents=True, exist_ok=True)
        progress_path.write_text(json.dumps(progress, indent=2) + "\n")

    save()
    env = dict(os.environ, RAYON_NUM_THREADS="1", OMP_NUM_THREADS="1")
    for height in args.heights:
        scale = SCALE_BY_HEIGHT[height]
        folder = root / "outputs" / tag / scale
        folder.mkdir(parents=True, exist_ok=True)
        output = folder / f"{scheme}.json"
        command = ["numactl", "--physcpubind=0", "--preferred=0", str(binary)]
        if scheme == "spiral_pbc":
            command += ["--height", str(height), "--warmup", str(args.warmups), "--trials", str(args.trials)]
        else:
            command += ["--input", str(args.treepir_root.resolve() / scale / "formal.json")]
        command += ["--trial-seed", str(args.trial_seed), "--output", str(output)]
        record = {"state": "waiting_idle", "command": command, "raw": str(output)}
        progress["heights"][str(height)] = record
        save()
        gate([sys.executable, str(root / "wait_idle.py"), "--root", str(root)], check=True)
        record["state"] = "running"
        save()
        process = runner(command, log_path=folder / f"{scheme}.log", env=env)
        (folder / f"{scheme}-process.json").write_text(json.dumps(process, indent=2) + "\n")
        record["process"] = process
        try:
            if process.get("exit_code") != 0:
                raise SpiralMatrixError(f"process exit {process.get('exit_code')}")
            record.update(validate(output, scheme=scheme, height=height, trials=args.trials,
                                   warmups=args.warmups, seed=args.trial_seed))
            record["state"] = "complete"
        except SpiralMatrixError as exc:
            record.update(state="failed", error=str(exc))
        save()
    ok = all(row["state"] == "complete" for row in progress["heights"].values())
    progress["state"] = "complete" if ok else "complete_with_failures"
    save()
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
