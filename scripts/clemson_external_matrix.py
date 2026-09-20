#!/usr/bin/env python3
"""Build and run the finite Clemson external full-path matrix.

The runner is intentionally serial: it waits for the shared host idle gate before
every build and adapter run, records failures, and continues with the remaining
backends. It does not fetch sources from the network; dependency roots must
already exist under ROOT/deps or in the staged source tree.
"""
from __future__ import annotations

import argparse
import datetime as dt
import json
import os
import re
import shutil
import subprocess
import sys
from dataclasses import dataclass
from pathlib import Path
from typing import Any, Callable


MODES = (
    "spirit",
    "spirit_rc",
    "vbpir_pbc",
    "inspire_layerwise",
    "treepir_spiral",
    "respire",
)
SCALE_BY_HEIGHT = {24: "1gib", 26: "4gib"}
TREEPIR_COMMIT = "930063c5aefc441244abb4890fdf35383f3aa956"
SPIRIT_COMMIT = "7e4c114807d5a83ae5cbad17c514df1dbc32dfc3"
INSPIRE_COMMIT = "533081a74301c8ba6ddd5e1dfc0c9daa6e3e75ef"
RESPIRE_COMMIT = "fd20d38e9e94c0298ab21af2f7699a23729eef67"


class MatrixError(Exception):
    pass


@dataclass
class CommandResult:
    exit_code: int
    stdout: str = ""
    stderr: str = ""


def now_utc() -> str:
    return dt.datetime.now(dt.timezone.utc).isoformat()


def write_json(path: Path, payload: Any) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(json.dumps(payload, indent=2, sort_keys=True) + "\n", encoding="utf-8")


def full_hex(value: str) -> bool:
    return re.fullmatch(r"[0-9a-f]{40}", value) is not None


def run_subprocess(command: list[str], *, cwd: Path, env: dict[str, str] | None = None) -> CommandResult:
    proc = subprocess.run(command, cwd=cwd, env=env, text=True, capture_output=True)
    return CommandResult(proc.returncode, proc.stdout, proc.stderr)


class MatrixRunner:
    def __init__(
        self,
        args: argparse.Namespace,
        *,
        command_runner: Callable[..., CommandResult] = run_subprocess,
    ) -> None:
        self.args = args
        self.root = args.root.resolve()
        self.source = self.root / "source"
        self.deps = self.root / "deps"
        self.outputs = self.root / "outputs" / "external-matrix"
        self.metadata = self.root / "metadata" / "external-matrix"
        self.wait_idle = args.wait_idle.resolve() if args.wait_idle else self.root / "wait_idle.py"
        self.command_runner = command_runner
        self.state: dict[str, Any] = {
            "state": "running",
            "updated_utc": now_utc(),
            "source_commit": args.source_commit,
            "source_root": str(self.source),
            "deps_root": str(self.deps),
            "hexl_root": str(args.hexl_root.resolve()),
            "heights": args.heights,
            "modes": args.modes,
            "events": [],
            "builds": {},
            "smoke": {},
            "formal": {},
            "vbpir_pilots": {},
            "gaps": {
                "treepir_sealpir": {
                    "state": "managed_separately",
                    "progress_json": str(self.root / "metadata" / "treepir-progress.json"),
                    "path_semantics": "legacy_colored_path_omits_root",
                    "reason": "TreePIR+SealPIR is handled by scripts/clemson_treepir_matrix.py as a separate existing VBPIR/SealPIR tuner with legacy colored-path semantics, not as a canonical root-included six-mode path-baseline row",
                }
            },
            "pins": {
                "treepir_vbpir_spiral": TREEPIR_COMMIT,
                "spirit": SPIRIT_COMMIT,
                "inspire_poulpy": INSPIRE_COMMIT,
                "respire": RESPIRE_COMMIT,
            },
        }

    def save(self) -> None:
        self.state["updated_utc"] = now_utc()
        write_json(self.metadata / "progress.json", self.state)

    def event(self, kind: str, payload: dict[str, Any]) -> None:
        self.state["events"].append({"at": now_utc(), "kind": kind, **payload})
        self.save()

    def base_env(self) -> dict[str, str]:
        env = os.environ.copy()
        env.update(
            {
                "OMP_NUM_THREADS": "1",
                "OPENBLAS_NUM_THREADS": "1",
                "MKL_NUM_THREADS": "1",
                "PIR_THREADS": "1",
                "RUSTC_BOOTSTRAP": "1",
                "CARGO_BUILD_JOBS": str(self.args.cargo_jobs),
                "BASELINE_WRAPPER_COMMIT": self.args.source_commit,
                "CARGO_TARGET_DIR": str(self.source / ".benchmarks" / "cargo-target-external"),
            }
        )
        return env

    def require_layout(self) -> None:
        if not self.args.dry_run:
            if not self.source.exists():
                raise MatrixError(f"missing source root: {self.source}")
            if not self.deps.exists():
                raise MatrixError(f"missing deps root: {self.deps}")
            if not self.wait_idle.exists():
                raise MatrixError(f"missing idle gate: {self.wait_idle}")
            if not self.args.hexl_root.exists():
                raise MatrixError(f"missing --hexl-root: {self.args.hexl_root}")
        self.outputs.mkdir(parents=True, exist_ok=True)
        self.metadata.mkdir(parents=True, exist_ok=True)

    def idle_gate(self, label: str) -> None:
        self.event("idle_gate", {"label": label, "command": [sys.executable, str(self.wait_idle), "--root", str(self.root)]})
        if self.args.dry_run:
            return
        result = self.command_runner([sys.executable, str(self.wait_idle), "--root", str(self.root)], cwd=self.root, env=self.base_env())
        if result.exit_code != 0:
            raise MatrixError(f"idle gate failed before {label}: {result.stderr.strip() or result.stdout.strip()}")

    def run_logged(
        self,
        name: str,
        command: list[str],
        *,
        cwd: Path | None = None,
        env: dict[str, str] | None = None,
        gate: bool = True,
    ) -> dict[str, Any]:
        if gate:
            self.idle_gate(name)
        log = self.metadata / "logs" / f"{name}.log"
        record = {"name": name, "command": [str(x) for x in command], "cwd": str(cwd or self.source), "log": str(log)}
        self.event("command_start", record)
        if self.args.dry_run:
            record.update(exit_code=0, dry_run=True)
            return record
        result = self.command_runner([str(x) for x in command], cwd=cwd or self.source, env=env or self.base_env())
        log.parent.mkdir(parents=True, exist_ok=True)
        log.write_text(
            "$ " + " ".join(record["command"]) + "\n\nSTDOUT:\n" + result.stdout + "\nSTDERR:\n" + result.stderr,
            encoding="utf-8",
        )
        record.update(exit_code=result.exit_code)
        if result.exit_code != 0:
            record["error"] = f"exit {result.exit_code}"
        self.event("command_done", record)
        return record

    def process_wrap(self, command: list[str], *, output: Path, log: Path, env: dict[str, str]) -> list[str]:
        return [
            sys.executable,
            str(self.source / "scripts" / "read_process_peak_memory.py"),
            "--log",
            str(log),
            "--output",
            str(output.with_name(f"{output.stem}-process.json")),
            "--",
            "numactl",
            "--physcpubind=0",
            "--preferred=0",
            *[str(x) for x in command],
        ]

    def hexl_dir(self) -> Path:
        for candidate in (
            self.args.hexl_root / "lib64" / "cmake" / "hexl-1.2.6",
            self.args.hexl_root / "lib" / "cmake" / "hexl-1.2.6",
        ):
            if candidate.exists() or self.args.dry_run:
                return candidate
        raise MatrixError(f"could not find HEXL cmake package under {self.args.hexl_root}")

    def ensure_inspire_stage(self) -> dict[str, Any]:
        target = self.source / ".benchmarks" / "inspire-path" / "poulpy-pir"
        source = self.deps / "poulpy-pir"
        record = {"target": str(target), "source": str(source), "state": "ready"}
        if self.args.dry_run:
            return record
        if target.exists():
            return record
        if not source.exists():
            raise MatrixError(f"missing Poulpy source for InsPIRe: {source}")
        target.parent.mkdir(parents=True, exist_ok=True)
        try:
            target.symlink_to(source, target_is_directory=True)
            record["method"] = "symlink"
        except OSError:
            shutil.copytree(source, target, symlinks=True)
            record["method"] = "copytree"
        return record

    def build_cmake(self, label: str, source_dir: Path, build_dir: Path, options: list[str]) -> tuple[bool, Path]:
        configure = self.run_logged(
            f"configure-{label}",
            ["cmake", "-S", str(source_dir), "-B", str(build_dir), "-DCMAKE_BUILD_TYPE=Release", *options],
        )
        if configure.get("exit_code") != 0:
            return False, build_dir
        build = self.run_logged(f"build-{label}", ["cmake", "--build", str(build_dir), "-j", str(self.args.build_jobs)])
        return build.get("exit_code") == 0, build_dir

    def build_spirit(self) -> tuple[bool, Path]:
        build_dir = self.root / "build-spirit-external"
        ok, _ = self.build_cmake(
            "spirit",
            self.source / "scripts" / "path_baselines" / "spirit",
            build_dir,
            [
                "-DSEAL_USE_INTEL_HEXL=ON",
                "-DSEAL_USE_MSGSL=OFF",
                f"-DSPIRIT_DIR={self.deps / 'SPIRIT'}",
                f"-DSPIRIT_SOURCE_COMMIT={SPIRIT_COMMIT}",
            ],
        )
        if ok:
            ctest = self.run_logged("ctest-spirit", ["ctest", "--test-dir", str(build_dir), "--output-on-failure"])
            ok = ctest.get("exit_code") == 0
        return ok, build_dir / "spirit_path_adapter"

    def build_vbpir(self) -> tuple[bool, Path]:
        stage = self.run_logged(
            "stage-vbpir",
            [
                "bash",
                str(self.source / "scripts" / "path_baselines" / "vbpir_pbc" / "stage_vbpir_pbc.sh"),
                str(self.deps / "TreePIR"),
                str(self.source / ".benchmarks" / "vbpir-pbc"),
            ],
        )
        if stage.get("exit_code") != 0:
            return False, self.root / "build-vbpir-external" / "vbpir_path_adapter"
        seal_dir = self.deps / "seal-install" / "lib" / "cmake" / "SEAL-4.0"
        ok, build_dir = self.build_cmake(
            "vbpir",
            self.source / "scripts" / "path_baselines" / "vbpir_pbc",
            self.root / "build-vbpir-external",
            [f"-DSEAL_DIR={seal_dir}", f"-DHEXL_DIR={self.hexl_dir()}"],
        )
        return ok, build_dir / "vbpir_path_adapter"

    def build_inspire(self) -> tuple[bool, Path]:
        try:
            stage = self.ensure_inspire_stage()
        except MatrixError as exc:
            self.state["builds"]["inspire_layerwise"] = {"state": "failed", "error": str(exc)}
            self.save()
            return False, self.source / ".benchmarks" / "inspire-path" / "target-external" / "release" / "inspire-path-adapter"
        self.state["builds"]["inspire_stage"] = stage
        env = self.base_env()
        env["CARGO_TARGET_DIR"] = str(self.source / ".benchmarks" / "inspire-path" / "target-external")
        env["CARGO_HOME"] = str(self.deps / "cargo-home")
        env["RUSTUP_HOME"] = str(self.deps / "rustup-home")
        env["PATH"] = str(self.deps / "cargo-home" / "bin") + os.pathsep + env.get("PATH", "")
        env["RUSTUP_TOOLCHAIN"] = "nightly-2026-05-14"
        env["INSPIRE_PIR_FEATURES"] = "avx512-fhe"
        env["RUSTFLAGS"] = "-C target-feature=+avx512f"
        record = self.run_logged(
            "build-inspire",
            [
                "cargo",
                "build",
                "--locked",
                "--release",
                "--manifest-path",
                str(self.source / "scripts" / "path_baselines" / "inspire" / "Cargo.toml"),
                "--features",
                "avx512-fhe",
            ],
            env=env,
        )
        return record.get("exit_code") == 0, Path(env["CARGO_TARGET_DIR"]) / "release" / "inspire-path-adapter"

    def build_respire(self) -> tuple[bool, Path]:
        env = self.base_env()
        env["RESPIRE_SOURCE_DIR"] = str(self.deps / "respire")
        stage = self.run_logged("stage-respire", ["bash", str(self.source / "scripts" / "path_baselines" / "respire" / "stage_respire.sh")], env=env)
        if stage.get("exit_code") != 0:
            return False, self.source / ".benchmarks" / "respire-path" / "cargo-target-external" / "release" / "path_workload"
        env["CARGO_TARGET_DIR"] = str(self.source / ".benchmarks" / "respire-path" / "cargo-target-external")
        env["RUSTFLAGS"] = "-C target-cpu=native"
        record = self.run_logged(
            "build-respire",
            [
                "cargo",
                "build",
                "--locked",
                "--release",
                "--manifest-path",
                str(self.source / ".benchmarks" / "respire-path" / "respire" / "Cargo.toml"),
                "--bin",
                "path_workload",
            ],
            env=env,
        )
        return record.get("exit_code") == 0, Path(env["CARGO_TARGET_DIR"]) / "release" / "path_workload"

    def build_spiral_height(self, height: int) -> tuple[bool, Path]:
        binary = self.source / ".benchmarks" / "spiral-path" / f"h{height}" / "build" / "Seperated" / "Seperated"
        record = self.run_logged(
            f"build-spiral-h{height}",
            [
                sys.executable,
                str(self.source / "scripts" / "path_baselines" / "treepir_spiral" / "build_spiral_cpu.py"),
                "--height",
                str(height),
                "--upstream-spiral",
                str(self.deps / "TreePIR" / "Spiral"),
                "--stage-root",
                str(self.source / ".benchmarks" / "spiral-path"),
                "--hexl-root",
                str(self.args.hexl_root),
                "--hexl-dir",
                str(self.hexl_dir()),
                "--jobs",
                str(self.args.build_jobs),
            ],
        )
        return record.get("exit_code") == 0, binary

    def build_backends(self) -> dict[str, Path]:
        binaries: dict[str, Path] = {}
        builders = {
            "spirit": self.build_spirit,
            "vbpir_pbc": self.build_vbpir,
            "inspire_layerwise": self.build_inspire,
            "respire": self.build_respire,
        }
        if not self.args.skip_build:
            for label, builder in builders.items():
                if label not in self.args.modes and not (label == "spirit" and "spirit_rc" in self.args.modes):
                    continue
                try:
                    ok, binary = builder()
                except Exception as exc:
                    ok, binary = False, Path("")
                    self.state["builds"][label] = {"state": "failed", "error": str(exc)}
                else:
                    self.state["builds"][label] = {"state": "complete" if ok else "failed", "binary": str(binary)}
                self.save()
                if ok:
                    binaries[label] = binary
                    if label == "spirit":
                        binaries["spirit_rc"] = binary
        else:
            binaries.update(
                {
                    "spirit": self.root / "build-spirit-external" / "spirit_path_adapter",
                    "spirit_rc": self.root / "build-spirit-external" / "spirit_path_adapter",
                    "vbpir_pbc": self.root / "build-vbpir-external" / "vbpir_path_adapter",
                    "inspire_layerwise": self.source / ".benchmarks" / "inspire-path" / "target-external" / "release" / "inspire-path-adapter",
                    "respire": self.source / ".benchmarks" / "respire-path" / "cargo-target-external" / "release" / "path_workload",
                }
            )
        if "treepir_spiral" in self.args.modes:
            for height in (8, *self.args.heights):
                if self.args.skip_build:
                    ok, binary = True, self.source / ".benchmarks" / "spiral-path" / f"h{height}" / "build" / "Seperated" / "Seperated"
                else:
                    try:
                        ok, binary = self.build_spiral_height(height)
                    except Exception as exc:
                        ok, binary = False, Path("")
                        self.state["builds"][f"treepir_spiral_h{height}"] = {"state": "failed", "error": str(exc)}
                    else:
                        self.state["builds"][f"treepir_spiral_h{height}"] = {"state": "complete" if ok else "failed", "binary": str(binary)}
                self.save()
                if ok:
                    binaries[f"treepir_spiral_h{height}"] = binary
        return binaries

    def leaf_file(self, folder: Path, height: int, warmups: int, trials: int) -> tuple[Path, list[int]]:
        sys.path.insert(0, str(self.source / "scripts"))
        from path_baselines.workload import trial_leaves

        leaves = trial_leaves(1 << height, warmups, trials, self.args.seed)
        path = folder / "leaves.txt"
        path.parent.mkdir(parents=True, exist_ok=True)
        path.write_text("".join(f"{leaf}\n" for leaf in leaves), encoding="utf-8")
        return path, leaves

    def adapter_command(self, mode: str, binary: Path, height: int, leaf_file: Path, warmups: int, trials: int, output: Path, extra: list[str]) -> list[str]:
        if mode == "treepir_spiral":
            binary_part = [sys.executable, str(self.source / "scripts" / "path_baselines" / "treepir_spiral" / "treepir_spiral_path_adapter.py")]
        elif binary.suffix == ".py":
            binary_part = [sys.executable, str(binary)]
        else:
            binary_part = [str(binary)]
        command = [
            *binary_part,
            "--height",
            str(height),
            "--leaf-file",
            str(leaf_file),
            "--warmup",
            str(warmups),
            "--trials",
            str(trials),
            "--output",
            str(output),
        ]
        if mode in ("spirit", "spirit_rc"):
            command += ["--scheme", mode]
        command += extra
        return command

    def validate_report(self, raw: Path, *, mode: str, height: int, leaves: list[int], warmups: int, trials: int) -> dict[str, Any]:
        sys.path.insert(0, str(self.source / "scripts"))
        from path_baselines.report import summarize_result, validate_result

        data = json.loads(raw.read_text(encoding="utf-8"))
        if data.get("scheme") != mode:
            raise MatrixError(f"{raw}: scheme {data.get('scheme')} != {mode}")
        validate_result(data, height=height, leaves=leaves, warmups=warmups, trials=trials)
        summary = summarize_result(data)
        process_path = raw.with_name(f"{raw.stem}-process.json")
        if process_path.exists():
            process = json.loads(process_path.read_text(encoding="utf-8"))
            summary["process_peak_rss_bytes"] = process.get("process_peak_rss_bytes")
        summary["raw"] = str(raw)
        return summary

    def run_adapter(self, mode: str, binary: Path, *, height: int, warmups: int, trials: int, folder: Path, extra: list[str]) -> tuple[dict[str, Any], dict[str, Any] | None]:
        leaf_file, leaves = self.leaf_file(folder, height, warmups, trials)
        raw = folder / f"{mode}.json"
        log = folder / f"{mode}.log"
        command = self.adapter_command(mode, binary, height, leaf_file, warmups, trials, raw, extra)
        wrapped = self.process_wrap(command, output=raw, log=log, env=self.base_env())
        record = {"mode": mode, "height": height, "trials": trials, "warmups": warmups, "raw": str(raw), "command": wrapped}
        run_record = self.run_logged(f"run-{mode}-h{height}-{folder.name}", wrapped, cwd=folder, env=self.base_env())
        record.update(exit_code=run_record.get("exit_code"), log=str(log))
        if record["exit_code"] != 0:
            record.update(state="failed", error=f"process exit {record['exit_code']}")
            return record, None
        if self.args.dry_run:
            record.update(state="planned", dry_run=True)
            return record, None
        try:
            summary = self.validate_report(raw, mode=mode, height=height, leaves=leaves, warmups=warmups, trials=trials)
        except Exception as exc:
            record.update(state="failed", error=str(exc))
            return record, None
        record["state"] = "complete"
        return record, summary

    def run_smokes(self, binaries: dict[str, Path]) -> None:
        for mode in self.args.modes:
            binary = binaries.get("treepir_spiral_h8") if mode == "treepir_spiral" else binaries.get(mode)
            if not binary:
                self.state["smoke"][mode] = {"state": "skipped", "reason": "backend build unavailable"}
                continue
            extra = self.extra_args(mode, height=8)
            record, summary = self.run_adapter(mode, binary, height=8, warmups=1, trials=2, folder=self.outputs / "smoke" / mode, extra=extra)
            self.state["smoke"][mode] = {**record, **({"summary": summary} if summary else {})}
            self.save()

    def extra_args(self, mode: str, *, height: int, first_dim: int | None = None) -> list[str]:
        if mode == "vbpir_pbc":
            return ["--first-dim", str(first_dim if first_dim is not None else 256)]
        if mode == "treepir_spiral":
            return [
                "--spiral-bin",
                str(self.source / ".benchmarks" / "spiral-path" / f"h{height}" / "build" / "Seperated" / "Seperated"),
                "--work-root",
                str(self.outputs / "spiral-work" / f"h{height}"),
            ]
        return []

    def run_vbpir_pilots(self, binaries: dict[str, Path], height: int) -> int | None:
        mode = "vbpir_pbc"
        if mode not in self.args.modes or mode not in binaries:
            return None
        pilot_rows = []
        for dim in (256, 512):
            record, summary = self.run_adapter(
                mode,
                binaries[mode],
                height=height,
                warmups=self.args.warmups,
                trials=self.args.vbpir_pilot_trials,
                folder=self.outputs / "vbpir-pilots" / SCALE_BY_HEIGHT[height] / f"fd{dim}",
                extra=self.extra_args(mode, height=height, first_dim=dim),
            )
            row = {"first_dim": dim, **record}
            if summary:
                row["server_ms"] = summary["server_ms"]
            pilot_rows.append(row)
            self.state["vbpir_pilots"][f"h{height}"] = pilot_rows
            self.save()
        complete = [row for row in pilot_rows if row.get("state") == "complete" and row.get("server_ms") is not None]
        return min(complete, key=lambda row: row["server_ms"])["first_dim"] if complete else None

    def run_formal(self, binaries: dict[str, Path]) -> None:
        for height in self.args.heights:
            scale = SCALE_BY_HEIGHT[height]
            selected_vbpir_dim = self.run_vbpir_pilots(binaries, height)
            for mode in self.args.modes:
                if self.state["smoke"].get(mode, {}).get("state") not in ("complete", "planned"):
                    self.state["formal"][f"{scale}:{mode}"] = {"state": "skipped", "reason": "smoke did not complete"}
                    self.save()
                    continue
                binary = binaries.get(mode)
                if mode == "treepir_spiral":
                    binary = binaries.get(f"treepir_spiral_h{height}")
                if not binary:
                    self.state["formal"][f"{scale}:{mode}"] = {"state": "skipped", "reason": "backend binary unavailable"}
                    self.save()
                    continue
                if mode == "vbpir_pbc" and selected_vbpir_dim is None:
                    self.state["formal"][f"{scale}:{mode}"] = {"state": "failed", "error": "no correct VBPIR pilot"}
                    self.save()
                    continue
                trials = self.args.treepir_trials if mode == "treepir_spiral" else self.args.trials
                first_dim = selected_vbpir_dim if mode == "vbpir_pbc" else None
                record, summary = self.run_adapter(
                    mode,
                    binary,
                    height=height,
                    warmups=self.args.warmups,
                    trials=trials,
                    folder=self.outputs / "formal" / scale / mode,
                    extra=self.extra_args(mode, height=height, first_dim=first_dim),
                )
                self.state["formal"][f"{scale}:{mode}"] = {**record, **({"summary": summary} if summary else {})}
                self.save()

    def write_summary(self) -> None:
        rows = []
        for key, record in sorted(self.state["formal"].items()):
            summary = record.get("summary")
            if not summary:
                continue
            rows.append(
                {
                    "scale_mode": key,
                    "samples": summary["completed_samples"],
                    "server_ms": summary["server_ms"],
                    "logical_mib_s": summary["logical_mib_s"],
                    "query_bytes": summary["query_bytes"],
                    "query_label": summary["communication"]["query"],
                    "response_bytes": summary["response_bytes"],
                    "response_label": summary["communication"]["response"],
                    "raw": summary["raw"],
                }
            )
        state = "complete" if self.state["formal"] and all(v.get("state") == "complete" for v in self.state["formal"].values()) else "complete_with_failures"
        self.state["state"] = "planned" if self.args.dry_run else state
        self.state["summary_rows"] = rows
        self.save()
        write_json(self.outputs / "summary.json", self.state)
        lines = [
            "# Clemson External Matrix",
            "",
            f"State: {self.state['state']}",
            "",
            "| Scale/mode | Samples | Server ms/path | Logical MiB/s | Query bytes | Response bytes | Raw |",
            "|---|---:|---:|---:|---:|---:|---|",
        ]
        for row in rows:
            lines.append(
                f"| {row['scale_mode']} | {row['samples']} | {row['server_ms']:.3f} | {row['logical_mib_s']:.2f} | "
                f"{row['query_bytes']:.0f} ({row['query_label']}) | {row['response_bytes']:.0f} ({row['response_label']}) | {row['raw']} |"
            )
        failures = [f"{key}: {record.get('error') or record.get('reason') or record.get('state')}" for key, record in sorted(self.state["formal"].items()) if record.get("state") != "complete"]
        if failures:
            lines += ["", "## Failed or skipped", "", *[f"- {item}" for item in failures]]
        treepir = self.state["gaps"]["treepir_sealpir"]
        lines += [
            "",
            "## Managed Separately",
            "",
            f"- TreePIR+SealPIR: {treepir['reason']}. Progress: {treepir['progress_json']}.",
        ]
        (self.outputs / "RESULTS.md").write_text("\n".join(lines) + "\n", encoding="utf-8")

    def run(self) -> dict[str, Any]:
        self.require_layout()
        self.save()
        binaries = self.build_backends()
        self.run_smokes(binaries)
        self.run_formal(binaries)
        self.write_summary()
        return self.state


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--root", type=Path, required=True)
    parser.add_argument("--source-commit", required=True)
    parser.add_argument("--hexl-root", type=Path, required=True)
    parser.add_argument("--wait-idle", type=Path)
    parser.add_argument("--heights", nargs="+", type=int, default=[24, 26])
    parser.add_argument("--modes", nargs="+", choices=MODES, default=list(MODES))
    parser.add_argument("--warmups", type=int, default=3)
    parser.add_argument("--trials", type=int, default=50)
    parser.add_argument("--treepir-trials", type=int, default=8)
    parser.add_argument("--vbpir-pilot-trials", type=int, default=8)
    parser.add_argument("--seed", type=int, default=7)
    parser.add_argument("--build-jobs", type=int, default=4)
    parser.add_argument("--cargo-jobs", type=int, default=2)
    parser.add_argument("--skip-build", action="store_true")
    parser.add_argument("--dry-run", action="store_true")
    args = parser.parse_args(argv)
    if not full_hex(args.source_commit):
        parser.error("--source-commit must be a full 40-character lowercase hex Git revision")
    for height in args.heights:
        if height not in SCALE_BY_HEIGHT:
            parser.error("--heights currently supports only 24 and 26 for finite 1GiB/4GiB matrix points")
    if args.warmups < 0 or args.trials <= 0 or args.treepir_trials <= 0 or args.vbpir_pilot_trials <= 0:
        parser.error("warmups must be nonnegative and trial counts must be positive")
    if args.treepir_trials > args.trials:
        parser.error("--treepir-trials must not exceed --trials")
    return args


def main(argv: list[str] | None = None) -> int:
    try:
        args = parse_args(argv)
        result = MatrixRunner(args).run()
        return 0 if result["state"] in ("complete", "planned") else 1
    except MatrixError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
