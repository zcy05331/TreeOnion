#!/usr/bin/env python3
"""Measurements on the paper's formal shape for the blind-review revision.

`noise` runs the evaluated parameters at 256 MiB to 8 GiB under three fresh
registrations each, writing the per-coordinate error moments and the raw error
of every payload coordinate of every measured query. `s128` runs a 128-bit
parameter point at every size, with two more registrations at 1 GiB. Every run
also appends 20 leaves to the preprocessed tree and checks the updated paths.
"""

import argparse
import json
import os
from pathlib import Path
import subprocess
import sys

from benchmark_tree_multiplane import Case, SweepError, select_layout, validate_result
from read_process_peak_memory import run_measured
from run_primary_matrix import SCALES, PrimaryMatrix, save

PAPER_N0 = {"256mib": 256}
LAYOUT = "split-columns"
TRIALS = 50
UPDATE_TRIALS = 20
PLANS = {
    "noise": ("build-ep3", 12,
              [(scale, seed) for scale in ("256mib", "1gib", "2gib", "4gib", "8gib")
               for seed in (7, 8, 9)]),
    "s128": ("build-s128", 8,
             [(scale, 7) for scale, _ in SCALES] + [("1gib", 8), ("1gib", 9)]),
}


class BlindReview(PrimaryMatrix):
    def __init__(self, root, plan):
        super().__init__(root, [scale for scale, _ in SCALES])
        binary_dir, chunk_bits, self.queue = PLANS[plan]
        self.binary = self.root / binary_dir / "Onion-PIR"
        self.g, self.planes = select_layout(256, chunk_bits)
        self.output = self.root / f"outputs/{plan}"
        self.progress = self.root / f"metadata/{plan}-progress.json"
        self.state.update(plan=plan, g=self.g, plane_count=self.planes,
                          n0={scale: PAPER_N0.get(scale, 512) for scale, _ in SCALES},
                          query_layout=LAYOUT, trials=TRIALS, update_trials=UPDATE_TRIALS,
                          queue=[f"{scale}/seed{seed}" for scale, seed in self.queue])
        for key in ("pilot_trials", "formal_trials_per_layout", "trial_seed"):
            self.state.pop(key, None)

    def run_one(self, scale, height, seed):
        n0 = PAPER_N0.get(scale, 512)
        folder = self.output / scale
        folder.mkdir(parents=True, exist_ok=True)
        label = f"g{self.g}-n0{n0}-seed{seed}"
        case = Case(label, self.g, n0, folder / f"{label}.json",
                    folder / f"{label}-process.json", folder / f"{label}.log")
        noise, errors = folder / f"{label}-noise.json", folder / f"{label}-errors.json"
        if any(p.exists() for p in (case.raw_json, case.process_json, case.log, noise, errors)):
            raise RuntimeError(f"refusing to overwrite an attempted case: {case.raw_json}")
        row = {"scale": scale, "seed": seed, "g": self.g, "N0": n0, "state": "waiting_idle",
               "raw_json": str(case.raw_json), "noise_json": str(noise),
               "errors_json": str(errors), "log": str(case.log)}
        self.state["cases"].append(row)
        self.update()
        subprocess.run([sys.executable, str(self.root / "wait_idle.py"),
                        "--root", str(self.root)], check=True)
        row["idle_before"] = json.loads((self.root / "metadata/idle.json").read_text())
        command = ["numactl", "--physcpubind=0", "--preferred=0", str(self.binary),
                   "--test", "tree_multiplane_bench", "--leaf-count", str(1 << height),
                   "--experiments", str(TRIALS), "--warmup", "3", "--trial-seed", str(seed),
                   "--benchmark-json", str(case.raw_json)]
        env = dict(os.environ, OMP_NUM_THREADS="1", OPENBLAS_NUM_THREADS="1",
                   MKL_NUM_THREADS="1", TREE_MULTIPLANE_ENTRY_BITS="256",
                   TREE_MULTIPLANE_G=str(self.g), TREE_MULTIPLANE_N0=str(n0),
                   TREE_MULTIPLANE_PACKING="mlwe", TREE_MULTIPLANE_QUERY_LAYOUT=LAYOUT,
                   TREE_MULTIPLANE_NOISE_STATS=str(noise),
                   TREE_MULTIPLANE_NOISE_DUMP=str(errors),
                   TREE_MULTIPLANE_UPDATE_TRIALS=str(UPDATE_TRIALS))
        row.update(command=command, state="running")
        self.update()
        process = run_measured(command, log_path=case.log, env=env)
        save(case.process_json, process)
        row["process"] = process
        try:
            if process["exit_code"]:
                raise SweepError(f"benchmark exited {process['exit_code']}; see log")
            row.update(validate_result(case.raw_json, case=case, leaf_count=1 << height,
                                       entry_bits=256, experiments=TRIALS, warmup=3,
                                       packing="mlwe", query_layout=LAYOUT, trial_seed=seed))
            meta = json.loads(case.raw_json.read_text())["metadata"]
            if meta.get("update_trials") != str(UPDATE_TRIALS):
                raise SweepError("update trials missing from the result")
            if not noise.exists() or not errors.exists():
                raise SweepError("noise outputs missing")
            row["state"] = "complete"
        except SweepError as error:
            row.update(state="failed", error=str(error))
        self.update()
        return row

    def run(self):
        if self.progress.exists():
            raise RuntimeError("queue already has progress; preserve it and inspect before retrying")
        self.update()
        heights = dict(SCALES)
        rows = [self.run_one(scale, heights[scale], seed) for scale, seed in self.queue]
        good = all(row["state"] == "complete" for row in rows)
        self.state["state"] = "complete" if good else "complete_with_failures"
        self.update()
        return 0 if good else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--plan", choices=sorted(PLANS), required=True)
    args = parser.parse_args()
    return BlindReview(args.root, args.plan).run()


if __name__ == "__main__":
    raise SystemExit(main())
