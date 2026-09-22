#!/usr/bin/env python3
"""Run the existing TreePIR backend tuner with the shared-host idle policy."""
import argparse
import json
from pathlib import Path
import subprocess
import sys

import run_treepir_backend_tuning as tuning
from read_process_peak_memory import run_measured


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--heights", nargs="+", type=int, default=[22, 23, 24, 25, 26])
    parser.add_argument("--candidates", nargs="+", help="forwarded to the backend tuner")
    parsed = parser.parse_args()
    root = parsed.root.resolve()
    args_heights = parsed.heights
    args = tuning.parse_args([
        "--run-root", str(root),
        "--vbpir-bin", str(root / "deps/TreePIR/VBPIR_TreePIR/build/bin/vbpir_treepir"),
        "--sealpir-binary", str(root / "build-treepir-sealpir/treepir_sealpir_adapter"),
        "--heights", *[str(h) for h in args_heights], "--trials", "8", "--pilot-trials", "8",
        "--warmups", "3", "--trial-seed", "7",
        *(["--candidates", *parsed.candidates] if parsed.candidates else [])])
    progress = {"state": "running", "heights": {}, "path_semantics": "legacy_colored_path_omits_root",
                "note": "VBPIR and SealPIRplus backend tuning for the retained TreePIR baseline"}
    status = root / "metadata/treepir-progress.json"

    def save():
        status.write_text(json.dumps(progress, indent=2) + "\n")

    def run(command, *, log_path, output_path, env):
        progress["current_command"] = command
        save()
        subprocess.run([sys.executable, str(root / "wait_idle.py"), "--root", str(root)], check=True)
        output_path.parent.mkdir(parents=True, exist_ok=True)
        process = run_measured(["numactl", "--physcpubind=0", "--preferred=0", *command],
                               log_path=log_path, env=env)
        output_path.with_name(output_path.stem + "-process.json").write_text(
            json.dumps(process, indent=2) + "\n")
        return process

    for height in args.heights:
        try:
            ok = tuning.run_one_height(args, height, run)
            progress["heights"][str(height)] = {"state": "complete" if ok else "failed"}
        except (tuning.TuningError, OSError, subprocess.CalledProcessError) as error:
            progress["heights"][str(height)] = {"state": "failed", "error": str(error)}
        save()
    ok = all(row["state"] == "complete" for row in progress["heights"].values())
    progress["state"] = "complete" if ok else "complete_with_failures"
    save()
    return 0 if ok else 1


if __name__ == "__main__":
    raise SystemExit(main())
