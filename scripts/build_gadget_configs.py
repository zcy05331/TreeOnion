#!/usr/bin/env python3
"""Build the bounded small-gadget experiment in an already configured build dir."""
import argparse
import json
from pathlib import Path
import shutil
import subprocess
import time


CONFIGS = {
    # EP rows, EP dropped bits, BV rows, BV dropped bits. KEY stays exact.
    "ep3-exact": (3, 0, 8, 0),
    "ep3-s24": (3, 24, 8, 0),
    "ep2-s30": (2, 30, 8, 0),
    "ks3-exact": (6, 0, 3, 0),
    "ep3-s32-ks3-s14": (3, 32, 3, 14),
    "ep2-s32-ks3-s14": (2, 32, 3, 14),
    "ks2-s20": (6, 0, 2, 20),
    "ep3-s32": (3, 32, 8, 0),
    "ep2-s36": (2, 36, 8, 0),
}


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument("--build-dir", type=Path, required=True)
    p.add_argument("--output", type=Path, required=True)
    p.add_argument("--binaries", type=Path, default=Path("build-gadget-configs"))
    p.add_argument("--jobs", type=int, default=4)
    p.add_argument("--config", action="append", choices=CONFIGS)
    args = p.parse_args()
    root = Path(__file__).resolve().parents[1]
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    if (out / "builds.json").exists():
        raise ValueError("choose a fresh output directory")
    manifest = {"state": "running", "builds": []}

    def save():
        (out / "builds.json").write_text(json.dumps(manifest, indent=2) + "\n")

    for label in args.config or CONFIGS:
        ep, ep_low, ks, ks_low = CONFIGS[label]
        row = {"label": label, "EP": ep, "EP_low_bits": ep_low,
               "KS": ks, "KS_low_bits": ks_low, "state": "building"}
        manifest["builds"].append(row)
        save()
        flags = dict(PIR_EP_ROWS=ep, PIR_EP_LOW_BITS=ep_low,
                     PIR_KS_ROWS=ks, PIR_KS_LOW_BITS=ks_low)
        command = ["cmake", "-S", str(root), "-B", str(args.build_dir.resolve())]
        command += [f"-D{k}={v}" for k, v in flags.items()]
        start = time.monotonic()
        with (out / f"{label}-build.log").open("w") as log:
            proc = subprocess.run(command, stdout=log, stderr=subprocess.STDOUT)
            if proc.returncode == 0:
                proc = subprocess.run(["cmake", "--build", str(args.build_dir.resolve()),
                                       "-j", str(args.jobs)], stdout=log, stderr=subprocess.STDOUT)
        row["elapsed_seconds"] = time.monotonic() - start
        row["exit_code"] = proc.returncode
        if proc.returncode:
            row["state"] = "failed"
        else:
            target = args.binaries.resolve() / label / "Onion-PIR"
            target.parent.mkdir(parents=True, exist_ok=True)
            shutil.copy2(args.build_dir / "Onion-PIR", target)
            row["binary"] = str(target)
            row["state"] = "built"
        save()
        print(json.dumps(row), flush=True)
    manifest["state"] = "complete" if all(x["state"] == "built" for x in manifest["builds"]) else "failed"
    save()
    return 0 if manifest["state"] == "complete" else 1


if __name__ == "__main__":
    raise SystemExit(main())
