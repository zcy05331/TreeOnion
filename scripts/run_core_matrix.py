#!/usr/bin/env python3
"""Run the finite core baseline matrix."""

from __future__ import annotations

import argparse
import json
import os
import shutil
import subprocess
import sys
from dataclasses import dataclass
from datetime import datetime, timezone
from pathlib import Path
from typing import Any


SCALES = {
    "256mb": 1 << 22,
    "512mb": 1 << 23,
    "1gb": 1 << 24,
    "2gb": 1 << 25,
    "4gb": 1 << 26,
    "8gb": 1 << 27,
}
SCALE_ORDER = ("256mb", "512mb", "1gb", "2gb", "4gb", "8gb")
# 8 GiB runs only when requested; the default matrix keeps the five smaller sizes.
DEFAULT_SCALES = SCALE_ORDER[:5]
CASES = ("standard", "layerwise", "layerwise_merged", "cuckoo", "flat")
# The default matrix keeps the four original baselines; the merged-response
# layerwise variant is an explicit opt-in.
DEFAULT_CASES = ("standard", "layerwise", "cuckoo", "flat")


def cuckoo_heights(scale: str, reference_expansion_height: int | None = None) -> tuple[int, ...]:
    # make_benchmark_reference chooses h=7 below 1 GiB and h=10 from 1 GiB.
    # The C++ Cuckoo benchmark rejects heights above that reference envelope,
    # so a pinned reference height widens (or narrows) the pilot range.
    envelope = reference_expansion_height
    if envelope is None:
        envelope = 7 if SCALES[scale] < (1 << 24) else 10
    return tuple(height for height in (7, 8, 9, 10) if height <= envelope)

WARMUPS = 3
TRIAL_SEED = 7
STANDARD_TRIALS = 50
CORE_TRIALS = 50
PILOT_TRIALS = 8
CUCKOO_TRIALS = 8
FLAT_TRIALS = 8

PATH_SEMANTICS = {
    "standard_reused": "single_plaintext_block",
    "standard": "single_plaintext_block",
    "tuning/layerwise/profile": "legacy_merkle_paths_omit_root",
    "layerwise_tuned": "legacy_merkle_paths_omit_root",
    "tuning/layerwise_merged/profile": "legacy_merkle_paths_omit_root",
    "layerwise_merged_tuned": "legacy_merkle_paths_omit_root",
    "flat": "legacy_merkle_paths_omit_root",
    "cuckoo_tuned": "legacy_merkle_paths_omit_root",
}


class MatrixError(Exception):
    pass


@dataclass(frozen=True)
class Paths:
    root: Path
    source: Path
    output: Path
    metadata: Path
    default_bin: Path
    ep3_bin: Path
    wait_idle: Path
    read_process: Path
    summarize: Path
    select_cuckoo: Path


def utc_now() -> str:
    return datetime.now(timezone.utc).replace(microsecond=0).isoformat().replace("+00:00", "Z")


def write_json(path: Path, data: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(data, indent=2, sort_keys=True) + "\n")


class Runner:
    def run(self, command: list[str], *, env: dict[str, str] | None = None, cwd: Path | None = None) -> subprocess.CompletedProcess[str]:
        return subprocess.run(command, env=env, cwd=cwd, text=True, capture_output=True, check=False)


class CoreMatrix:
    def __init__(
        self,
        paths: Paths,
        *,
        standard_1g_source: Path | None = None,
        runner: Runner | None = None,
        scales: tuple[str, ...] = DEFAULT_SCALES,
        cases: tuple[str, ...] = DEFAULT_CASES,
        reference_expansion_height: int | None = None,
        gadget: str = "exact",
    ) -> None:
        self.paths = paths
        # Every case runs on one build: exact EP6 (build-default) or the
        # 3-row approximate gadget of the tree scheme (build-ep3).
        self.gadget = gadget
        self.binary = paths.ep3_bin if gadget == "ep3" else paths.default_bin
        self.standard_1g_source = standard_1g_source
        self.runner = runner or Runner()
        self.scales = tuple(scales)
        self.cases = tuple(cases)
        # When set, every Merkle-derived case (standard, layerwise, cuckoo,
        # flat) pins the reference expansion height so all scales share one
        # first-dimension geometry instead of the smallest feasible height.
        self.reference_expansion_height = reference_expansion_height
        self.status_path = paths.output / "status.json"
        self.rows: list[dict[str, Any]] = []

    def save_status(self, state: str = "running") -> None:
        write_json(
            self.status_path,
            {
                "schema_version": "run_core_matrix.v1",
                "state": state,
                "root": str(self.paths.root),
                "source": str(self.paths.source),
                "output": str(self.paths.output),
                "default_binary": str(self.paths.default_bin),
                "ep3_binary": str(self.paths.ep3_bin),
                "gadget": self.gadget,
                "binary": str(self.binary),
                "scales": list(self.scales),
                "cases": list(self.cases),
                "reference_expansion_height": self.reference_expansion_height,
                "rows": self.rows,
            },
        )

    def append_row(self, scale: str, case: str, status: str, **extra: Any) -> dict[str, Any]:
        path_semantics = PATH_SEMANTICS.get(case)
        if path_semantics is None and case.startswith("tuning/cuckoo/"):
            path_semantics = "legacy_merkle_paths_omit_root"
        if path_semantics is None:
            path_semantics = "not_applicable"
        row = {
            "scale": scale,
            "case": case,
            "status": status,
            "path_semantics": path_semantics,
            "utc": utc_now(),
            **extra,
        }
        self.rows.append(row)
        self.save_status()
        return row

    def update_row(self, row: dict[str, Any], status: str, **extra: Any) -> None:
        row["status"] = status
        row["utc"] = utc_now()
        row.update(extra)
        self.save_status()

    def validate_profile(self, json_path: Path, *, trials: int, leaf_count: int) -> subprocess.CompletedProcess[str]:
        return self.runner.run(
            [
                sys.executable,
                str(self.paths.summarize),
                "--validate",
                str(json_path),
                "--trials",
                str(trials),
                "--leaf-count",
                str(leaf_count),
            ],
            cwd=self.paths.source,
        )

    def run_wait_idle(self) -> subprocess.CompletedProcess[str]:
        command = [sys.executable, str(self.paths.wait_idle), "--root", str(self.paths.root)]
        return self.runner.run(command, cwd=self.paths.root)

    def measured_call(
        self,
        *,
        scale: str,
        case: str,
        command: list[str],
        json_path: Path,
        process_path: Path,
        log_path: Path,
        trials: int,
        env: dict[str, str] | None = None,
        row_extra: dict[str, Any] | None = None,
    ) -> bool:
        leaf_count = SCALES[scale]
        json_path.parent.mkdir(parents=True, exist_ok=True)
        process_path.parent.mkdir(parents=True, exist_ok=True)
        log_path.parent.mkdir(parents=True, exist_ok=True)
        row = self.append_row(
            scale,
            case,
            "running",
            command=command,
            json=str(json_path),
            process=str(process_path),
            log=str(log_path),
            trials=trials,
            **(row_extra or {}),
        )
        wait = self.run_wait_idle()
        row["wait_idle"] = {
            "command": [sys.executable, str(self.paths.wait_idle), "--root", str(self.paths.root)],
            "exit_code": wait.returncode,
            "stdout": wait.stdout,
            "stderr": wait.stderr,
        }
        self.save_status()
        if wait.returncode != 0:
            self.update_row(row, "failed", error="wait_idle failed", exit_code=wait.returncode)
            return False
        wrapper = [
            sys.executable,
            str(self.paths.read_process),
            "--log",
            str(log_path),
            "--output",
            str(process_path),
            "--",
            "numactl",
            "--physcpubind=0",
            "--preferred=0",
            *command,
        ]
        merged_env = os.environ.copy()
        if env:
            merged_env.update(env)
        result = self.runner.run(wrapper, env=merged_env, cwd=self.paths.source)
        row["wrapper_command"] = wrapper
        row["wrapper_exit_code"] = result.returncode
        row["wrapper_stdout"] = result.stdout
        row["wrapper_stderr"] = result.stderr
        if result.returncode != 0:
            self.update_row(row, "failed", error="benchmark process failed", exit_code=result.returncode)
            return False
        validation = self.validate_profile(json_path, trials=trials, leaf_count=leaf_count)
        row["validation"] = {
            "exit_code": validation.returncode,
            "stdout": validation.stdout,
            "stderr": validation.stderr,
        }
        if validation.returncode != 0:
            self.update_row(row, "failed", error="profile validation failed", exit_code=validation.returncode)
            return False
        self.update_row(row, "complete", exit_code=0)
        return True

    def validate_process_success(self, path: Path) -> None:
        try:
            process = json.loads(path.read_text())
        except FileNotFoundError as exc:
            raise MatrixError(f"{path}: missing process JSON") from exc
        except json.JSONDecodeError as exc:
            raise MatrixError(f"{path}: invalid process JSON: {exc}") from exc
        if process.get("exit_code") != 0:
            raise MatrixError(f"{path}: exit_code is not 0")

    def reuse_standard_1gb(self) -> None:
        if self.standard_1g_source is None:
            self.standard("1gb")
            return
        scale = "1gb"
        folder = self.paths.output / scale
        folder.mkdir(parents=True, exist_ok=True)
        source_json = self.standard_1g_source / "standard.json"
        source_process = self.standard_1g_source / "standard-process.json"
        target_json = folder / "standard.json"
        target_process = folder / "standard-process.json"
        row = self.append_row(
            scale,
            "standard_reused",
            "running",
            source_json=str(source_json),
            source_process=str(source_process),
            json=str(target_json),
            process=str(target_process),
            trials=STANDARD_TRIALS,
        )
        try:
            self.validate_process_success(source_process)
            validation = self.validate_profile(source_json, trials=STANDARD_TRIALS, leaf_count=SCALES[scale])
            row["validation"] = {
                "exit_code": validation.returncode,
                "stdout": validation.stdout,
                "stderr": validation.stderr,
            }
            if validation.returncode != 0:
                raise MatrixError("source standard 1GiB validation failed")
            shutil.copy2(source_json, target_json)
            shutil.copy2(source_process, target_process)
        except MatrixError as exc:
            self.update_row(row, "failed", error=str(exc), exit_code=1)
            return
        self.update_row(row, "complete", exit_code=0)

    def geometry_args(self) -> list[str]:
        if self.reference_expansion_height is None:
            return []
        return ["--reference-expansion-height", str(self.reference_expansion_height)]

    def geometry_row(self) -> dict[str, Any]:
        if self.reference_expansion_height is None:
            return {}
        return {"reference_expansion_height": self.reference_expansion_height}

    def standard(self, scale: str) -> bool:
        folder = self.paths.output / scale
        leaf_count = SCALES[scale]
        geometry = self.geometry_args()
        row_extra = self.geometry_row()
        return self.measured_call(
            scale=scale,
            case="standard",
            command=[
                str(self.binary),
                "--test",
                "merkle_benchmarks",
                "--benchmark-case",
                "standard_onionpir",
                "--leaf-count",
                str(leaf_count),
                "--experiments",
                str(STANDARD_TRIALS),
                "--warmup",
                str(WARMUPS),
                "--trial-seed",
                str(TRIAL_SEED),
                *geometry,
                "--benchmark-json",
                str(folder / "standard.json"),
            ],
            json_path=folder / "standard.json",
            process_path=folder / "standard-process.json",
            log_path=folder / "standard.log",
            trials=STANDARD_TRIALS,
            row_extra=row_extra or None,
        )

    def layerwise_profile(self, scale: str, *, merged: bool = False) -> bool:
        variant = "layerwise_merged" if merged else "layerwise"
        folder = self.paths.output / scale / "tuning" / variant
        profile = folder / "profile.json"
        process_path = folder / "profile-process.json"
        log_path = folder / "profile.log"
        leaf_count = SCALES[scale]
        folder.mkdir(parents=True, exist_ok=True)
        row = self.append_row(
            scale,
            f"tuning/{variant}/profile",
            "running",
            json=str(profile),
            process=str(process_path),
            log=str(log_path),
            trials=PILOT_TRIALS,
            **self.geometry_row(),
        )
        wait = self.run_wait_idle()
        row["wait_idle"] = {"exit_code": wait.returncode, "stdout": wait.stdout, "stderr": wait.stderr}
        self.save_status()
        if wait.returncode != 0:
            self.update_row(row, "failed", error="wait_idle failed", exit_code=wait.returncode)
            return False
        wrapper = [
            sys.executable,
            str(self.paths.read_process),
            "--log",
            str(log_path),
            "--output",
            str(process_path),
            "--",
            "numactl",
            "--physcpubind=0",
            "--preferred=0",
            str(self.binary),
            "--test",
            "layer_layout_sweep",
            "--leaf-count",
            str(leaf_count),
            "--experiments",
            str(PILOT_TRIALS),
            "--warmup",
            str(WARMUPS),
            "--trial-seed",
            str(TRIAL_SEED),
            "--layer-padding-budget",
            "1.1",
            *(["--layer-nodes-per-plaintext", "64"] if merged else []),
            *self.geometry_args(),
            "--layout-profile-json",
            str(profile),
        ]
        result = self.runner.run(wrapper, cwd=self.paths.source)
        row["wrapper_command"] = wrapper
        row["wrapper_exit_code"] = result.returncode
        row["wrapper_stdout"] = result.stdout
        row["wrapper_stderr"] = result.stderr
        if result.returncode != 0:
            self.update_row(row, "failed", error="layer profile process failed", exit_code=result.returncode)
            return False
        try:
            self.validate_layer_profile(profile, process_path, leaf_count)
        except MatrixError as exc:
            self.update_row(row, "failed", error=str(exc), exit_code=1)
            return False
        self.update_row(row, "complete", exit_code=0)
        return True

    def validate_layer_profile(self, profile_path: Path, process_path: Path, leaf_count: int) -> None:
        self.validate_process_success(process_path)
        try:
            profile = json.loads(profile_path.read_text())
        except FileNotFoundError as exc:
            raise MatrixError(f"{profile_path}: missing profile JSON") from exc
        except json.JSONDecodeError as exc:
            raise MatrixError(f"{profile_path}: invalid profile JSON: {exc}") from exc
        if profile.get("measured_trials") != PILOT_TRIALS:
            raise MatrixError(f"{profile_path}: measured_trials != {PILOT_TRIALS}")
        if profile.get("warmups") != WARMUPS:
            raise MatrixError(f"{profile_path}: warmups != {WARMUPS}")
        if profile.get("tree_height") != (leaf_count.bit_length() - 1):
            raise MatrixError(f"{profile_path}: tree_height mismatch")
        measurements = profile.get("measurements")
        if not isinstance(measurements, list) or not measurements:
            raise MatrixError(f"{profile_path}: missing measurements")

    def layerwise_tuned(self, scale: str, *, merged: bool = False) -> bool:
        folder = self.paths.output / scale
        leaf_count = SCALES[scale]
        variant = "layerwise_merged" if merged else "layerwise"
        profile = folder / "tuning" / variant / "profile.json"
        case = f"{variant}_tuned"
        return self.measured_call(
            scale=scale,
            case=case,
            command=[
                str(self.binary),
                "--test",
                "merkle_benchmarks",
                "--benchmark-case",
                "merkle_layerwise_merged" if merged else "merkle_layerwise",
                "--leaf-count",
                str(leaf_count),
                "--experiments",
                str(CORE_TRIALS),
                "--warmup",
                str(WARMUPS),
                "--trial-seed",
                str(TRIAL_SEED),
                "--layer-layout-policy",
                "profiled",
                "--layer-layout-profile",
                str(profile),
                "--layer-padding-budget",
                "1.1",
                *self.geometry_args(),
                "--benchmark-json",
                str(folder / f"{case}.json"),
            ],
            json_path=folder / f"{case}.json",
            process_path=folder / f"{case}-process.json",
            log_path=folder / f"{case}.log",
            trials=CORE_TRIALS,
            row_extra=self.geometry_row() or None,
        )

    def flat(self, scale: str) -> bool:
        folder = self.paths.output / scale
        leaf_count = SCALES[scale]
        return self.measured_call(
            scale=scale,
            case="flat",
            command=[
                str(self.binary),
                "--test",
                "merkle_benchmarks",
                "--benchmark-case",
                "merkle_paths",
                "--leaf-count",
                str(leaf_count),
                "--experiments",
                str(FLAT_TRIALS),
                "--warmup",
                str(WARMUPS),
                "--trial-seed",
                str(TRIAL_SEED),
                *self.geometry_args(),
                "--benchmark-json",
                str(folder / "flat.json"),
            ],
            json_path=folder / "flat.json",
            process_path=folder / "flat-process.json",
            log_path=folder / "flat.log",
            trials=FLAT_TRIALS,
            row_extra={
                "primary_case": "merkle_flat",
                "cli_case_selection": "merkle_paths",
                "cli_case_note": "existing merkle_paths selection emits merkle_flat and merkle_layerwise; use merkle_flat as the Flat primary row",
                **self.geometry_row(),
            },
        )

    def cuckoo_pilot(self, scale: str, height: int) -> bool:
        folder = self.paths.output / scale / "tuning" / "cuckoo"
        leaf_count = SCALES[scale]
        json_path = folder / f"h{height}.json"
        return self.measured_call(
            scale=scale,
            case=f"tuning/cuckoo/h{height}",
            command=[
                str(self.binary),
                "--test",
                "cuckoo_bench",
                "--leaf-count",
                str(leaf_count),
                "--experiments",
                str(PILOT_TRIALS),
                "--warmup",
                str(WARMUPS),
                "--trial-seed",
                str(TRIAL_SEED),
                *self.geometry_args(),
            ],
            json_path=json_path,
            process_path=folder / f"h{height}-process.json",
            log_path=folder / f"h{height}.log",
            trials=PILOT_TRIALS,
            env={"BENCHMARK_OUTPUT_JSON": str(json_path), "CUCKOO_LAYOUT_HEIGHT": str(height)},
            row_extra=self.geometry_row() or None,
        )

    def heights(self, scale: str) -> tuple[int, ...]:
        return cuckoo_heights(scale, self.reference_expansion_height)

    def select_cuckoo(self, scale: str) -> int | None:
        folder = self.paths.output / scale / "tuning" / "cuckoo"
        output = folder / "selection.json"
        row = self.append_row(scale, "tuning/cuckoo/selection", "running", json=str(output))
        command = [
            sys.executable,
            str(self.paths.select_cuckoo),
            "--output",
            str(output),
            "--expected-trials",
            str(PILOT_TRIALS),
            "--expected-warmups",
            str(WARMUPS),
            "--leaf-count",
            str(SCALES[scale]),
            "--heights",
            ",".join(map(str, self.heights(scale))),
            *[str(folder / f"h{height}.json") for height in self.heights(scale)],
        ]
        result = self.runner.run(command, cwd=self.paths.source)
        row["command"] = command
        row["stdout"] = result.stdout
        row["stderr"] = result.stderr
        if result.returncode != 0:
            self.update_row(row, "failed", error="cuckoo selection failed", exit_code=result.returncode)
            return None
        try:
            height = int(result.stdout.strip())
        except ValueError:
            self.update_row(row, "failed", error="cuckoo selector did not print an integer height", exit_code=1)
            return None
        self.update_row(row, "complete", exit_code=0, chosen_height=height)
        return height

    def cuckoo_tuned(self, scale: str, height: int) -> bool:
        folder = self.paths.output / scale
        leaf_count = SCALES[scale]
        json_path = folder / "cuckoo_tuned.json"
        return self.measured_call(
            scale=scale,
            case="cuckoo_tuned",
            command=[
                str(self.binary),
                "--test",
                "cuckoo_bench",
                "--leaf-count",
                str(leaf_count),
                "--experiments",
                str(CUCKOO_TRIALS),
                "--warmup",
                str(WARMUPS),
                "--trial-seed",
                str(TRIAL_SEED),
                *self.geometry_args(),
            ],
            json_path=json_path,
            process_path=folder / "cuckoo_tuned-process.json",
            log_path=folder / "cuckoo_tuned.log",
            trials=CUCKOO_TRIALS,
            env={"BENCHMARK_OUTPUT_JSON": str(json_path), "CUCKOO_LAYOUT_HEIGHT": str(height)},
            row_extra=self.geometry_row() or None,
        )

    def run(self) -> int:
        self.paths.output.mkdir(parents=True, exist_ok=True)
        self.paths.metadata.mkdir(parents=True, exist_ok=True)
        self.save_status()
        for scale in self.scales:
            if "standard" in self.cases:
                # A pinned reference geometry must be measured, not copied from
                # a reference run made under the default rule.
                if scale == "1gb" and self.reference_expansion_height is None:
                    self.reuse_standard_1gb()
                else:
                    self.standard(scale)
            if "layerwise" in self.cases:
                if self.layerwise_profile(scale):
                    self.layerwise_tuned(scale)
                else:
                    self.append_row(scale, "layerwise_tuned", "skipped", reason="layerwise profile failed")
            if "layerwise_merged" in self.cases:
                if self.layerwise_profile(scale, merged=True):
                    self.layerwise_tuned(scale, merged=True)
                else:
                    self.append_row(scale, "layerwise_merged_tuned", "skipped", reason="layerwise_merged profile failed")
            if "cuckoo" in self.cases:
                pilots_ok = True
                for height in self.heights(scale):
                    pilots_ok = self.cuckoo_pilot(scale, height) and pilots_ok
                if pilots_ok:
                    chosen = self.select_cuckoo(scale)
                    if chosen is not None:
                        self.cuckoo_tuned(scale, chosen)
                else:
                    self.append_row(scale, "tuning/cuckoo/selection", "skipped", reason="one or more pilots failed")
                    self.append_row(scale, "cuckoo_tuned", "skipped", reason="cuckoo selection unavailable")
            if "flat" in self.cases:
                self.flat(scale)
        failed = [row for row in self.rows if row["status"] == "failed"]
        self.save_status("failed" if failed else "complete")
        return 1 if failed else 0


def make_paths(root: Path, output: Path | None) -> Paths:
    source = root / "source"
    out = output or (root / "outputs" / "core-matrix")
    return Paths(
        root=root,
        source=source,
        output=out,
        metadata=root / "metadata",
        default_bin=root / "build-default" / "Onion-PIR",
        ep3_bin=root / "build-ep3" / "Onion-PIR",
        wait_idle=root / "wait_idle.py",
        read_process=source / "scripts" / "read_process_peak_memory.py",
        summarize=source / "scripts" / "summarize_cloudlab_profiles.py",
        select_cuckoo=source / "scripts" / "select_cuckoo_layout.py",
    )


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--output-dir", type=Path)
    parser.add_argument("--standard-1gb-source", type=Path)
    parser.add_argument("--scales", nargs="+", choices=SCALE_ORDER, default=list(DEFAULT_SCALES))
    parser.add_argument("--cases", nargs="+", choices=CASES, default=list(DEFAULT_CASES))
    parser.add_argument("--gadget", choices=("exact", "ep3"), default="exact",
                        help="selector gadget build: exact EP6 (build-default) or ep3-s32 (build-ep3)")
    parser.add_argument(
        "--reference-expansion-height",
        type=int,
        choices=range(1, 11),
        help="pin the Merkle reference expansion height for every case instead of the default rule",
    )
    return parser.parse_args(argv)


def main(argv: list[str] | None = None) -> int:
    args = parse_args(argv)
    paths = make_paths(args.root.resolve(), args.output_dir.resolve() if args.output_dir else None)
    matrix = CoreMatrix(
        paths,
        standard_1g_source=args.standard_1gb_source.resolve() if args.standard_1gb_source else None,
        scales=tuple(scale for scale in SCALE_ORDER if scale in args.scales),
        cases=tuple(case for case in CASES if case in args.cases),
        reference_expansion_height=args.reference_expansion_height,
        gadget=args.gadget,
    )
    return matrix.run()


if __name__ == "__main__":
    raise SystemExit(main())
