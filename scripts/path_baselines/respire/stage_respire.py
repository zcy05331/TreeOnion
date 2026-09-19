#!/usr/bin/env python3
"""Stage a verified Respire checkout and install the path workload adapter."""
from __future__ import annotations

import argparse
import shutil
import subprocess
from pathlib import Path

SOURCE_COMMIT = "fd20d38e9e94c0298ab21af2f7699a23729eef67"
UPSTREAM_URL = "https://github.com/AMACB/respire.git"


def run(command: list[str], cwd: Path) -> subprocess.CompletedProcess:
    return subprocess.run(command, cwd=cwd, text=True, capture_output=True, check=True)


def git_stdout(command: list[str], cwd: Path) -> str:
    return run(command, cwd).stdout.strip()


def require_git_checkout(path: Path, *, clean: bool) -> None:
    try:
        inside = git_stdout(["git", "rev-parse", "--is-inside-work-tree"], path)
    except subprocess.CalledProcessError as exc:
        raise SystemExit(f"{path} is not a git checkout") from exc
    if inside != "true":
        raise SystemExit(f"{path} is not a git checkout")
    head = git_stdout(["git", "rev-parse", "HEAD"], path)
    if head != SOURCE_COMMIT:
        raise SystemExit(f"{path} HEAD {head} != expected {SOURCE_COMMIT}")
    if clean and git_stdout(["git", "status", "--porcelain"], path):
        raise SystemExit(f"{path} must be clean before staging")


def apply_patch_if_needed(respire_dir: Path, patch: Path) -> None:
    check = subprocess.run(
        ["git", "apply", "--check", str(patch)],
        cwd=respire_dir,
        text=True,
        capture_output=True,
    )
    if check.returncode == 0:
        run(["git", "apply", str(patch)], respire_dir)
        return
    reverse = subprocess.run(
        ["git", "apply", "--reverse", "--check", str(patch)],
        cwd=respire_dir,
        text=True,
        capture_output=True,
    )
    if reverse.returncode == 0:
        return
    raise SystemExit(f"{patch} is neither applicable nor already applied")


def copy_adapter(adapter_dir: Path, respire_dir: Path) -> None:
    destination = respire_dir / "src" / "bin" / "path_workload"
    if destination.exists():
        shutil.rmtree(destination)
    shutil.copytree(adapter_dir / "path_workload", destination)


def stage(adapter_dir: Path, stage_root: Path, source_dir: Path | None) -> Path:
    respire_dir = stage_root / "respire"
    patch = adapter_dir / "patches" / "respire_timing_boundaries.patch"
    stage_root.mkdir(parents=True, exist_ok=True)
    if not (respire_dir / "Cargo.toml").exists():
        if source_dir is None:
            run(["git", "clone", UPSTREAM_URL, str(respire_dir)], adapter_dir)
            run(["git", "checkout", SOURCE_COMMIT], respire_dir)
        else:
            source_dir = source_dir.resolve()
            require_git_checkout(source_dir, clean=True)
            shutil.copytree(source_dir, respire_dir)
    require_git_checkout(respire_dir, clean=False)
    apply_patch_if_needed(respire_dir, patch)
    copy_adapter(adapter_dir, respire_dir)
    return respire_dir


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser()
    parser.add_argument("--adapter-dir", type=Path, required=True)
    parser.add_argument("--stage-root", type=Path, required=True)
    parser.add_argument("--source-dir", type=Path)
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    print(stage(args.adapter_dir.resolve(), args.stage_root.resolve(), args.source_dir))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
