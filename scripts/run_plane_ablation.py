#!/usr/bin/env python3
"""Plane-geometry ablation: the policy shape (g = 8, P = 3) against one plane
(g = 32, P = 1), with everything else fixed to the paper's formal rows."""

import argparse
from pathlib import Path

from benchmark_tree_multiplane import select_layout
from run_primary_matrix import SCALES, PrimaryMatrix

# N0 and query-buffer layout of the paper's formal rows at each size.
PAPER_N0 = {"256mib": 256}
PAPER_LAYOUT = {"8gib": "interleaved"}
SINGLE_PLANE_G = 32


class PlaneAblation(PrimaryMatrix):
    """Both shapes run back to back on each size with 50 measured queries on
    the same leaf schedule, so the pair differs only in the plane geometry."""

    def __init__(self, root, scales):
        super().__init__(root, scales)
        self.output = self.root / "outputs/plane-ablation"
        self.progress = self.root / "metadata/plane-ablation-progress.json"
        self.policy_g, policy_planes = select_layout(256)
        self.state.update(layout_policy="ablation: policy shape against one plane",
                          g=[self.policy_g, SINGLE_PLANE_G], plane_count=[policy_planes, 1],
                          n0={scale: PAPER_N0.get(scale, 512) for scale in self.scales},
                          query_layout={scale: PAPER_LAYOUT.get(scale, "split-columns")
                                        for scale in self.scales})
        for key in ("pilot_trials", "formal_trials_per_layout"):
            self.state.pop(key, None)
        self.state["trials_per_shape"] = 50

    def run(self):
        if self.progress.exists():
            raise RuntimeError("ablation queue already has progress; preserve it and inspect before retrying")
        self.update()
        good = True
        for scale, height in SCALES:
            if scale not in self.scales:
                continue
            n0 = PAPER_N0.get(scale, 512)
            layout = PAPER_LAYOUT.get(scale, "split-columns")
            rows = [self.run_case(scale, height, "ablation", g, n0, layout, 50,
                                  policy_layout=g == self.policy_g)
                    for g in (self.policy_g, SINGLE_PLANE_G)]
            complete = [row for row in rows if row["state"] == "complete"]
            if len(complete) == 2 and complete[0]["sample_leaves"] != complete[1]["sample_leaves"]:
                raise RuntimeError("ablation schedules differ")
            good = good and len(complete) == 2
        self.state["state"] = "complete" if good else "complete_with_failures"
        self.update()
        return 0 if good else 1


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--scales", nargs="+", choices=[scale for scale, _ in SCALES],
                        default=[scale for scale, _ in SCALES])
    args = parser.parse_args()
    return PlaneAblation(args.root, args.scales).run()


if __name__ == "__main__":
    raise SystemExit(main())
