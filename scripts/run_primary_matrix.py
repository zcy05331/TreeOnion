#!/usr/bin/env python3
"""Tune N0 for the fixed h1 layout, then measure both query-buffer layouts."""

import argparse
import datetime as dt
import json
import os
from pathlib import Path
import subprocess
import sys

from benchmark_tree_multiplane import Case, SweepError, select_layout, validate_result
from read_process_peak_memory import run_measured


def save(path, data):
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2) + "\n")


def candidate_shapes():
    g, _ = select_layout(256)
    return [(g, n0) for n0 in (512, 256, 1024, 128)]


class PrimaryMatrix:
    def __init__(self, root):
        self.root = root.resolve()
        self.binary = self.root / "build-ep3/Onion-PIR"
        self.output = self.root / "outputs/primary"
        self.progress = self.root / "metadata/primary-progress.json"
        self.state = {"state": "running", "source_commit":
                      (self.root / "metadata/source-commit.txt").read_text().strip(),
                      "index_layout": "gamma-alpha-beta", "entry_bits": 256,
                      "layout_policy": "maximum utilization, minimum P, P<=6",
                      "g": 8, "plane_count": 3,
                      "warmups": 3, "trial_seed": 7, "pilot_trials": 8,
                      "formal_trials_per_layout": 50, "cases": [], "selection": {}}

    def update(self):
        self.state["updated_utc"] = dt.datetime.now(dt.timezone.utc).isoformat()
        save(self.progress, self.state)

    def run_case(self, scale, height, phase, g, n0, layout, trials):
        folder = self.output / scale / phase
        folder.mkdir(parents=True, exist_ok=True)
        label = f"g{g}-n0{n0}-{layout}"
        case = Case(label, g, n0, folder / f"{label}.json",
                    folder / f"{label}-process.json", folder / f"{label}.log")
        if any(p.exists() for p in (case.raw_json, case.process_json, case.log)):
            raise RuntimeError(f"refusing to overwrite an attempted case: {case.raw_json}")
        row = {"scale": scale, "phase": phase, "g": g, "N0": n0,
               "query_layout": layout, "trials": trials, "state": "waiting_idle",
               "raw_json": str(case.raw_json), "process_json": str(case.process_json),
               "log": str(case.log)}
        self.state["cases"].append(row)
        self.update()
        subprocess.run([sys.executable, str(self.root / "wait_idle.py"),
                        "--root", str(self.root)], check=True)
        # Keep the pre-case observation, not merely the latest gate state.
        row["idle_before"] = json.loads((self.root / "metadata/idle.json").read_text())
        command = ["numactl", "--physcpubind=0", "--preferred=0", str(self.binary),
                   "--test", "tree_multiplane_bench", "--leaf-count", str(1 << height),
                   "--experiments", str(trials), "--warmup", "3", "--trial-seed", "7",
                   "--benchmark-json", str(case.raw_json)]
        env = dict(os.environ, OMP_NUM_THREADS="1", OPENBLAS_NUM_THREADS="1",
                   MKL_NUM_THREADS="1", TREE_MULTIPLANE_ENTRY_BITS="256",
                   TREE_MULTIPLANE_G=str(g), TREE_MULTIPLANE_N0=str(n0),
                   TREE_MULTIPLANE_PACKING="mlwe", TREE_MULTIPLANE_QUERY_LAYOUT=layout)
        row.update(command=command, state="running")
        self.update()
        process = run_measured(command, log_path=case.log, env=env)
        save(case.process_json, process)
        row["process"] = process
        try:
            if process["exit_code"]:
                raise SweepError(f"benchmark exited {process['exit_code']}; see log")
            row.update(validate_result(case.raw_json, case=case, leaf_count=1 << height,
                       entry_bits=256, experiments=trials, warmup=3, packing="mlwe",
                       query_layout=layout, trial_seed=7))
            row["state"] = "complete"
        except SweepError as error:
            row.update(state="failed", error=str(error))
        self.update()
        return row

    def run(self):
        if self.progress.exists():
            raise RuntimeError("primary queue already has progress; preserve it and inspect before retrying")
        self.update()
        scales = (("256mib", 22), ("512mib", 23), ("1gib", 24), ("2gib", 25), ("4gib", 26))
        for scale, height in scales:
            pilots = [self.run_case(scale, height, "pilots", g, n0, "interleaved", 8)
                      for g, n0 in candidate_shapes()]
            valid = [row for row in pilots if row["state"] == "complete"]
            if not valid:
                self.state["selection"][scale] = {"state": "failed", "reason": "no correct pilot"}
                self.update()
                continue
            schedule = valid[0]["sample_leaves"]
            for row in valid:
                if row["sample_leaves"] != schedule:
                    raise RuntimeError("pilot schedules differ; refusing a mismatched comparison")
            best = min(valid, key=lambda row: row["mean_server_total_ms"])
            self.state["selection"][scale] = {"g": best["g"], "N0": best["N0"],
                "pilot_mean_server_ms": best["mean_server_total_ms"], "pilot_json": best["raw_json"],
                "criterion": "minimum mean full server time at the structurally selected g/P"}
            self.update()
            # Both query-buffer layouts get 50 full trials at identical selected
            # parameters. Keep both rows; do not attribute a g/N0 change to tiling.
            formal = [self.run_case(scale, height, "formal", best["g"], best["N0"], layout, 50)
                      for layout in ("interleaved", "split-columns")]
            complete = [row for row in formal if row["state"] == "complete"]
            if len(complete) == 2 and complete[0]["sample_leaves"] != complete[1]["sample_leaves"]:
                raise RuntimeError("formal A/B schedules differ")
            self.state["selection"][scale]["formal_state"] = (
                "complete" if len(complete) == 2 else "failed")
            self.update()
        good = all(self.state["selection"].get(s, {}).get("formal_state") == "complete"
                   for s, _ in scales)
        self.state["state"] = "complete" if good else "complete_with_failures"
        self.update()
        return 0 if good else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    args = parser.parse_args()
    return PrimaryMatrix(args.root).run()


if __name__ == "__main__":
    raise SystemExit(main())
