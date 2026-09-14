#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import pathlib
import subprocess
import sys
import time
from typing import Any, Callable

TERMINAL_STATUSES = {"complete", "complete_with_failures", "failed"}
VALID_STATUSES = TERMINAL_STATUSES | {"running"}
SSH_OPTIONS = [
    "-o", "BatchMode=yes",
    "-o", "ConnectTimeout=10",
    "-o", "ServerAliveInterval=15",
    "-o", "ServerAliveCountMax=2",
]
RSYNC_SSH = "ssh " + " ".join(SSH_OPTIONS)

Runner = Callable[..., subprocess.CompletedProcess]
Sleeper = Callable[[float], None]


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description="Collect completed CloudLab stage results once as progress advances.")
    parser.add_argument("--host", required=True, help="SSH host name for the CloudLab node")
    parser.add_argument("--remote-root", required=True, help="Remote run root containing metadata/ and outputs/")
    parser.add_argument("--local-root", required=True, type=pathlib.Path, help="Local directory for collected results")
    parser.add_argument("--interval", type=float, default=60.0, help="Polling interval in seconds; default: 60")
    return parser.parse_args(argv)


def remote_join(root: str, *parts: str) -> str:
    return "/".join([root.rstrip("/"), *[part.strip("/") for part in parts]])


def load_collection(local_root: pathlib.Path) -> dict[str, Any]:
    path = local_root / "collection.json"
    if not path.exists():
        return {"copied_stages": [], "status": "initializing", "terminal": False, "unreachable": False}
    with path.open("r", encoding="utf-8") as handle:
        data = json.load(handle)
    data.setdefault("copied_stages", [])
    data.setdefault("status", "initializing")
    data.setdefault("terminal", False)
    data.setdefault("unreachable", False)
    return data


def save_collection(local_root: pathlib.Path, collection: dict[str, Any]) -> None:
    local_root.mkdir(parents=True, exist_ok=True)
    path = local_root / "collection.json"
    tmp = path.with_suffix(".json.tmp")
    with tmp.open("w", encoding="utf-8") as handle:
        json.dump(collection, handle, indent=2, sort_keys=True)
        handle.write("\n")
    tmp.replace(path)


def read_remote_progress(host: str, remote_root: str, runner: Runner) -> tuple[dict[str, Any] | None, str | None]:
    progress_path = remote_join(remote_root, "metadata", "progress.json")
    result = runner(["ssh", *SSH_OPTIONS, host, "cat", progress_path], capture_output=True, text=True)
    if result.returncode != 0:
        message = (result.stderr or result.stdout or "metadata/progress.json unavailable").strip()
        if "No such file" in message or "not found" in message or "No such" in message:
            return None, "waiting_for_progress"
        return None, message or "ssh failed"
    try:
        progress = json.loads(result.stdout)
    except json.JSONDecodeError as exc:
        return None, f"invalid progress json: {exc}"
    status = progress.get("status")
    if status not in VALID_STATUSES:
        return None, f"invalid progress status: {status!r}"
    return progress, None


def rsync_path(host: str, remote_path: str, local_path: pathlib.Path, runner: Runner) -> tuple[bool, str | None]:
    local_path.mkdir(parents=True, exist_ok=True)
    command = ["rsync", "-az", "--no-perms", "-e", RSYNC_SSH, f"{host}:{remote_path.rstrip('/')}/", str(local_path) + "/"]
    result = runner(command, capture_output=True, text=True)
    if result.returncode != 0:
        return False, (result.stderr or result.stdout or "rsync failed").strip()
    return True, None


def stages_from_progress(progress: dict[str, Any]) -> list[str]:
    stages: list[str] = []
    for stage in progress.get("finished_stages", []) or []:
        if stage not in stages:
            stages.append(stage)
    return stages


def update_error(collection: dict[str, Any], message: str) -> None:
    errors = collection.setdefault("errors", [])
    errors.append(message)
    collection["last_error"] = message


def collect_once(args: argparse.Namespace, collection: dict[str, Any], runner: Runner) -> tuple[bool, bool]:
    progress, error = read_remote_progress(args.host, args.remote_root, runner)
    if progress is None:
        collection["status"] = error or "unreachable"
        if error != "waiting_for_progress":
            update_error(collection, collection["status"])
            return False, False
        return True, False

    collection["status"] = progress["status"]
    collection["last_progress"] = progress

    ok, error = rsync_path(args.host, remote_join(args.remote_root, "metadata"), args.local_root / "metadata", runner)
    if not ok:
        update_error(collection, f"metadata sync failed: {error}")
        return False, False

    copied = list(collection.get("copied_stages", []))
    for stage in stages_from_progress(progress):
        if stage in copied:
            continue
        ok, error = rsync_path(
            args.host,
            remote_join(args.remote_root, "outputs", stage),
            args.local_root / "outputs" / stage,
            runner,
        )
        if not ok:
            update_error(collection, f"stage {stage} sync failed: {error}")
            return False, False
        copied.append(stage)
        collection["copied_stages"] = copied

    terminal = progress["status"] in TERMINAL_STATUSES
    collection["terminal"] = terminal
    collection["unreachable"] = False
    return True, terminal


def main(argv: list[str] | None = None, *, runner: Runner = subprocess.run, sleeper: Sleeper = time.sleep) -> int:
    args = parse_args(argv)
    args.local_root.mkdir(parents=True, exist_ok=True)
    collection = load_collection(args.local_root)
    consecutive_failures = 0

    while True:
        ok, terminal = collect_once(args, collection, runner)
        if ok:
            consecutive_failures = 0
        else:
            consecutive_failures += 1
            if consecutive_failures >= 3:
                collection["unreachable"] = True
                collection["terminal"] = False
                save_collection(args.local_root, collection)
                return 2

        save_collection(args.local_root, collection)
        if terminal:
            return 0
        sleeper(args.interval)


if __name__ == "__main__":
    sys.exit(main())
