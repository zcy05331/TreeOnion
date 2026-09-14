#!/usr/bin/env python3
"""Finite native MLWE response-packing comparison launcher."""
import argparse
import datetime
import json
import shutil
import socket
import subprocess
import sys
from pathlib import Path


SHAPES = [
    ("h3", "3,64,1,352,9", 9),
    ("h1", "1,8,3,512,7", 10),
]


def utc_now():
    return datetime.datetime.now(datetime.timezone.utc).isoformat()


def write_hardware(path):
    path.parent.mkdir(parents=True, exist_ok=True)
    commands = [
        ["date", "-u"],
        ["uname", "-a"],
        ["hostname"],
        ["lscpu"],
        ["numactl", "--hardware"],
        ["free", "-h"],
        ["c++", "--version"],
        ["cmake", "--version"],
    ]
    with path.open("w") as output:
        output.write(f"python {sys.version}\n")
        output.write(f"socket_hostname {socket.gethostname()}\n")
        for command in commands:
            output.write("\n$ " + " ".join(command) + "\n")
            try:
                subprocess.run(command, stdout=output, stderr=subprocess.STDOUT)
            except FileNotFoundError as error:
                output.write(f"missing command: {error.filename}\n")


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--hexl", required=True)
    parser.add_argument("--source-commit", required=True)
    args = parser.parse_args()

    root = args.root.resolve()
    source = root / "source"
    outputs = root / "outputs"
    metadata = root / "metadata"
    binaries = root / "binaries"
    build_default = root / "build-default"
    build_mlwe = root / "build-ep2-drop36"
    progress_path = metadata / "progress.json"

    if not (source / "CMakeLists.txt").exists():
        raise RuntimeError("--root/source/CMakeLists.txt must exist before launching")
    if build_default.exists() or build_mlwe.exists():
        raise RuntimeError("choose a fresh --root without build directories")
    outputs.mkdir(parents=True, exist_ok=True)
    metadata.mkdir(parents=True, exist_ok=True)
    binaries.mkdir(parents=True, exist_ok=True)

    state = {
        "status": "running",
        "source_commit": args.source_commit,
        "source_root": str(source),
        "build_default": str(build_default),
        "build_ep2_drop36": str(build_mlwe),
        "finished_stages": [],
        "comparison_states": {},
        "failures": [],
    }

    def save():
        state["updated_utc"] = utc_now()
        progress_path.write_text(json.dumps(state, indent=2) + "\n")

    def stage(name, command, *, cwd=None, allow_failure=False):
        state["current_stage"] = name
        save()
        log_path = outputs / f"{name}.log"
        with log_path.open("w") as log:
            proc = subprocess.run(command, cwd=cwd, stdout=log, stderr=subprocess.STDOUT)
        record = {
            "name": name,
            "command": command,
            "cwd": str(cwd) if cwd else None,
            "log": str(log_path),
            "exit_code": proc.returncode,
        }
        state["finished_stages"].append(record)
        if proc.returncode:
            state["failures"].append(record)
            save()
            if not allow_failure:
                raise RuntimeError(f"{name} failed; see {log_path}")
        save()
        return proc.returncode

    try:
        save()
        write_hardware(metadata / "hardware.txt")

        default_configure = [
            "cmake",
            "-S",
            str(source),
            "-B",
            str(build_default),
            "-DACTIVE_CONFIG=CONFIG_N2048_K1_COMP",
            "-DHEXL_DIR=" + args.hexl,
            "-DCMAKE_BUILD_TYPE=Benchmark",
            "-DPIR_EP_ROWS=6",
            "-DPIR_EP_LOW_BITS=0",
            "-DPIR_KS_ROWS=8",
            "-DPIR_KS_LOW_BITS=0",
        ]
        stage("configure_default_ep6_drop0_ks8_drop0", default_configure)
        stage("build_default", ["cmake", "--build", str(build_default), "-j", "4"])
        stage("test_default_all", [str(build_default / "Onion-PIR"), "--test", "all"])

        mlwe_configure = [
            "cmake",
            "-S",
            str(source),
            "-B",
            str(build_mlwe),
            "-DACTIVE_CONFIG=CONFIG_N2048_K1_COMP",
            "-DHEXL_DIR=" + args.hexl,
            "-DCMAKE_BUILD_TYPE=Benchmark",
            "-DPIR_EP_ROWS=2",
            "-DPIR_EP_LOW_BITS=36",
            "-DPIR_KS_ROWS=8",
            "-DPIR_KS_LOW_BITS=0",
        ]
        stage("configure_ep2_drop36_ks8_drop0", mlwe_configure)
        stage("build_ep2_drop36", ["cmake", "--build", str(build_mlwe), "-j", "4"])
        candidate = binaries / "ep2-drop36" / "Onion-PIR"
        candidate.parent.mkdir(parents=True, exist_ok=True)
        shutil.copy2(build_mlwe / "Onion-PIR", candidate)
        state["benchmark_binary"] = str(candidate)
        save()
        stage("test_tree_response_merge", [str(candidate), "--test", "tree_response_merge"])
        stage("test_tree_response_merge_e2e", [str(candidate), "--test", "tree_response_merge_e2e"])

        for shape_name, shape, height in SHAPES:
            benchmark_out = outputs / f"{shape_name}-packing"
            command = [
                "numactl",
                "--physcpubind=0",
                "--interleave=all",
                sys.executable,
                "scripts/benchmark_gadget_configs.py",
                "--output",
                str(benchmark_out),
                "--experiments",
                "50",
                "--rounds",
                "2",
                "--leaf-count",
                str(1 << 24),
                "--shape",
                shape,
                "--expansion-height",
                str(height),
                "--memory-gib",
                "80",
                "--binary",
                f"projection={candidate}",
                "--source",
                f"projection={args.source_commit}",
                "--packing",
                "projection=projection",
                "--binary",
                f"mlwe={candidate}",
                "--source",
                f"mlwe={args.source_commit}",
                "--packing",
                "mlwe=mlwe",
            ]
            exit_code = stage(f"benchmark_{shape_name}_projection_mlwe", command, cwd=source, allow_failure=True)
            comparison = benchmark_out / "comparison.json"
            if comparison.exists():
                result = json.loads(comparison.read_text())
                state["comparison_states"][shape_name] = result.get("state", "missing_state")
            else:
                state["comparison_states"][shape_name] = "failed_no_comparison_json"
            if exit_code:
                state["comparison_states"][shape_name] = "failed"
            save()

        states = state.get("comparison_states", {})
        state["status"] = "complete" if states and all(value == "complete" for value in states.values()) else "complete_with_failures"
        state["current_stage"] = None
        save()
    except Exception as error:
        state["status"] = "failed"
        state["error"] = str(error)
        state["current_stage"] = None
        save()
        raise


if __name__ == "__main__":
    main()
