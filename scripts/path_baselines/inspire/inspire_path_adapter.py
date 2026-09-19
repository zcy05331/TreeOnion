#!/usr/bin/env python3
"""Run the isolated Rust Poulpy InsPIRe path adapter with the common CLI."""
import os
import subprocess
import sys
import time
from pathlib import Path


ROOT = Path(__file__).resolve().parents[3]
MANIFEST = ROOT / "scripts" / "path_baselines" / "inspire" / "Cargo.toml"
TARGET = ROOT / ".benchmarks" / "inspire-path" / "target"
RUNS = ROOT / ".benchmarks" / "inspire-path" / "runs"


def absolutize_adapter_paths(argv):
    out = list(argv)
    for flag in ("--leaf-file", "--output"):
        if flag not in out:
            continue
        idx = out.index(flag) + 1
        if idx >= len(out):
            continue
        out[idx] = str(Path(out[idx]).expanduser().resolve())
    return out


def run_dir():
    RUNS.mkdir(parents=True, exist_ok=True)
    stamp = f"{int(time.time() * 1_000_000)}-{os.getpid()}"
    path = RUNS / stamp
    path.mkdir()
    return path


def main(argv):
    env = os.environ.copy()
    env.setdefault("RUSTC_BOOTSTRAP", "1")
    env.setdefault("CARGO_TARGET_DIR", str(TARGET))
    command = [
        "cargo",
        "run",
        "--release",
        "--quiet",
        "--locked",
        "--manifest-path",
        str(MANIFEST),
    ]
    features = env.get("INSPIRE_PIR_FEATURES", "").strip()
    if features:
        command += ["--features", features]
    command += ["--", *absolutize_adapter_paths(argv)]
    return subprocess.run(command, cwd=run_dir(), env=env).returncode


if __name__ == "__main__":
    raise SystemExit(main(sys.argv[1:]))
