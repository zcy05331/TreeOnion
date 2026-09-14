#!/usr/bin/env python3
"""Tune TreePIR backend variants after the core profile matrix finishes."""

from __future__ import annotations

import argparse
import json
import math
import os
import subprocess
import sys
from pathlib import Path
from typing import Any, Callable


SUPPORTED_HEIGHTS = {24: "1gb", 26: "4gb", 27: "8gb"}
DEFAULT_VBPIR_DIMS = (64, 128, 256)
H27_VBPIR_DIMS = (128, 256)
SEALPIR_DIMS = (1, 2)


class TuningError(Exception):
    pass


class Candidate:
    def __init__(self, label: str, backend: str, params: dict[str, int]):
        self.label = label
        self.backend = backend
        self.params = params


class CandidateResult:
    def __init__(self, label: str, eligible: bool, mean_server_ms: float | None, reason: str):
        self.label = label
        self.eligible = eligible
        self.mean_server_ms = mean_server_ms
        self.reason = reason

    def as_dict(self) -> dict[str, Any]:
        return {
            "label": self.label,
            "eligible": self.eligible,
            "mean_server_ms": self.mean_server_ms,
            "reason": self.reason,
        }

    def raise_if_ineligible(self, path: Path) -> None:
        if not self.eligible:
            raise TuningError(f"{path}: {self.label} is ineligible: {self.reason}")


def scale_for_height(height: int) -> str:
    if height not in SUPPORTED_HEIGHTS:
        raise TuningError(f"unsupported TreePIR height: {height}")
    return SUPPORTED_HEIGHTS[height]


def vbpir_first_dims(height: int) -> tuple[int, ...]:
    return H27_VBPIR_DIMS if height == 27 else DEFAULT_VBPIR_DIMS


def candidates_for_height(height: int) -> list[Candidate]:
    return [
        *(Candidate(f"vbpir_fd{dim}", "vbpir", {"first_dim": dim}) for dim in vbpir_first_dims(height)),
        *(Candidate(f"sealpir_d{dim}", "sealpir", {"pir_d": dim}) for dim in SEALPIR_DIMS),
    ]


def load_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text())
    except (OSError, json.JSONDecodeError) as exc:
        raise TuningError(f"{path}: cannot load JSON: {exc}") from exc


def finite_positive(value: Any) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(float(value)) and float(value) > 0.0


def finite_nonnegative(value: Any) -> bool:
    return isinstance(value, (int, float)) and not isinstance(value, bool) and math.isfinite(float(value)) and float(value) >= 0.0


def validate_leaf_schedule(path: Path, *, trials: int, warmups: int, leaf_count: int) -> list[int]:
    data = load_json(path)
    schedule = data.get("leaf_schedule")
    if not isinstance(schedule, dict):
        raise TuningError(f"{path}: missing leaf_schedule")
    warmup = schedule.get("warmup")
    measured = schedule.get("measured")
    if not isinstance(warmup, list) or len(warmup) != warmups:
        raise TuningError(f"{path}: warmup leaf count does not match {warmups}")
    if not isinstance(measured, list) or len(measured) != trials:
        raise TuningError(f"{path}: measured leaf count does not match {trials}")
    leaves = [*warmup, *measured]
    for leaf in leaves:
        if not isinstance(leaf, int) or isinstance(leaf, bool) or leaf < 0 or leaf >= leaf_count:
            raise TuningError(f"{path}: leaf {leaf!r} is outside [0, {leaf_count})")
    return leaves


def write_leaf_file(path: Path, leaves: list[int]) -> None:
    path.write_text("".join(f"{leaf}\n" for leaf in leaves))


def validate_index_only(path: Path, *, height: int, trials: int, warmups: int, leaves: list[int]) -> None:
    data = load_json(path)
    if data.get("validate_index_only") is not True:
        raise TuningError(f"{path}: index-only output missing validate_index_only=true")
    if data.get("height") not in (height, None):
        raise TuningError(f"{path}: height {data.get('height')} does not match {height}")
    if data.get("leaf_count") not in (1 << height, None):
        raise TuningError(f"{path}: leaf_count does not match height")
    results = data.get("results")
    if not isinstance(results, list) or len(results) != warmups + trials:
        raise TuningError(f"{path}: index-only result count does not match warmup+trials")
    if [result.get("leaf_id") for result in results] != leaves:
        raise TuningError(f"{path}: index-only leaf schedule does not match generated leaf list")
    measured = [result for result in results if result.get("phase") == "measured"]
    if len(measured) != trials:
        raise TuningError(f"{path}: index-only measured result count does not match {trials}")
    for index, result in enumerate(results):
        if result.get("correct") is not True:
            raise TuningError(f"{path}: index-only result {index} correctness is not true")
        if not isinstance(result.get("path_nodes"), list) or not result["path_nodes"]:
            raise TuningError(f"{path}: result {index} missing path_nodes")
        if not isinstance(result.get("sub_indices"), list) or not result["sub_indices"]:
            raise TuningError(f"{path}: result {index} missing sub_indices")


def measured_results(data: dict[str, Any]) -> list[dict[str, Any]]:
    results = data.get("results")
    if not isinstance(results, list):
        return []
    return [result for result in results if result.get("phase") == "measured"]


def validate_candidate_result(candidate: Candidate, data: dict[str, Any], *, height: int, trials: int, warmups: int,
                              leaves: list[int] | None = None) -> CandidateResult:
    if data.get("height") not in (height, None):
        return CandidateResult(candidate.label, False, None, f"height {data.get('height')} does not match {height}")
    if data.get("leaf_count") not in (1 << height, None):
        return CandidateResult(candidate.label, False, None, "leaf_count does not match height")
    measured = measured_results(data)
    if len(measured) != trials:
        return CandidateResult(candidate.label, False, None, f"measured result count {len(measured)} does not match {trials}")
    results = data.get("results")
    if leaves is not None:
        if not isinstance(results, list) or len(results) != warmups + trials:
            return CandidateResult(candidate.label, False, None, "leaf schedule result count does not match warmup+trials")
        if [result.get("leaf_id") for result in results] != leaves:
            return CandidateResult(candidate.label, False, None, "leaf schedule does not match generated leaf list")
    if data.get("warmups") not in (warmups, None):
        return CandidateResult(candidate.label, False, None, f"warmups {data.get('warmups')} does not match {warmups}")
    if data.get("trials") not in (trials, None):
        return CandidateResult(candidate.label, False, None, f"trials {data.get('trials')} does not match {trials}")
    summary = data.get("measured_summary")
    if isinstance(summary, dict):
        if summary.get("all_correct") is not True:
            return CandidateResult(candidate.label, False, None, "summary correctness is not true")
        if isinstance(summary.get("count"), int) and summary["count"] != trials:
            return CandidateResult(candidate.label, False, None, f"summary count {summary['count']} does not match {trials}")
    values: list[float] = []
    for index, result in enumerate(measured):
        if result.get("correct") is not True:
            return CandidateResult(candidate.label, False, None, f"result {index} correctness is not true")
        if not finite_positive(result.get("server_ms")):
            return CandidateResult(candidate.label, False, None, f"result {index} server_ms is not finite positive")
        for stage in ("index_ms", "query_ms", "decode_ms", "server_first_dimension_ms", "server_second_dimension_ms",
                      "server_last_dimension_ms", "server_merge_ms", "response_serialize_ms", "query_serialize_ms"):
            if result.get(stage) is not None and not finite_nonnegative(result[stage]):
                return CandidateResult(candidate.label, False, None, f"result {index} {stage} is not finite nonnegative")
        values.append(float(result["server_ms"]))
    return CandidateResult(candidate.label, True, math.fsum(values) / len(values), "ok")


def select_best_candidate(results: list[CandidateResult]) -> CandidateResult:
    eligible = [result for result in results if result.eligible and result.mean_server_ms is not None and math.isfinite(result.mean_server_ms)]
    if not eligible:
        raise TuningError("no eligible TreePIR backend candidates")
    return min(eligible, key=lambda result: result.mean_server_ms)


def run_process(command: list[str], *, log_path: Path, output_path: Path, env: dict[str, str]) -> dict[str, Any]:
    output_path.parent.mkdir(parents=True, exist_ok=True)
    log_path.parent.mkdir(parents=True, exist_ok=True)
    wrapper = Path(env["RUN_ROOT"]) / "source" / "scripts" / "read_process_peak_memory.py"
    full_command = [
        sys.executable,
        str(wrapper),
        "--log",
        str(log_path),
        "--output",
        str(output_path.with_name(f"{output_path.stem}-process.json")),
        "--",
        "numactl",
        "--physcpubind=0",
        "--interleave=all",
        *command,
    ]
    completed = subprocess.run(full_command, env=env, text=True, capture_output=True, check=False)
    if completed.stdout:
        with log_path.open("a") as log:
            log.write(completed.stdout)
    if completed.stderr:
        with log_path.open("a") as log:
            log.write(completed.stderr)
    process_path = output_path.with_name(f"{output_path.stem}-process.json")
    if process_path.exists():
        return load_json(process_path)
    return {"exit_code": completed.returncode}


def process_ok(process: dict[str, Any]) -> bool:
    return process.get("exit_code") == 0


def command_for_candidate(candidate: Candidate, *, vbpir: Path, sealpir: Path, height: int, leaf_file: Path,
                          index_json: Path, output: Path, warmups: int, trials: int) -> list[str]:
    if candidate.backend == "vbpir":
        return [str(vbpir), "--height", str(height), "--leaf-file", str(leaf_file), "--warmup", str(warmups),
                "--trials", str(trials), "--output", str(output), "--first-dim", str(candidate.params["first_dim"])]
    return [str(sealpir), "--input", str(index_json), "--output", str(output), "--warmup", str(warmups),
            "--trials", str(trials), "--pir-d", str(candidate.params["pir_d"])]


def run_pilot_candidates(args, height, runner, *, tuning_dir, leaves, leaf_file,
                         index_json, vbpir, sealpir, env) -> list[CandidateResult]:
    """Evaluate each backend on the pilot prefix of the shared leaf schedule."""
    candidate_results: list[CandidateResult] = []
    pilot_leaves = leaves[:args.warmups + args.pilot_trials]
    for candidate in candidates_for_height(height):
        output = tuning_dir / f"pilot-{candidate.label}.json"
        log = tuning_dir / f"pilot-{candidate.label}.log"
        command = command_for_candidate(candidate, vbpir=vbpir, sealpir=sealpir, height=height, leaf_file=leaf_file,
                                        index_json=index_json, output=output, warmups=args.warmups,
                                        trials=args.pilot_trials)
        process = runner(command, log_path=log, output_path=output, env=env)
        if not process_ok(process):
            candidate_results.append(CandidateResult(candidate.label, False, None, f"process exit {process.get('exit_code')}"))
            continue
        candidate_results.append(validate_candidate_result(candidate, load_json(output), height=height,
                                                          trials=args.pilot_trials, warmups=args.warmups,
                                                          leaves=pilot_leaves))

    return candidate_results


def run_formal_candidate(args, height, runner, *, folder, tuning_dir, scale, leaves, leaf_file,
                         index_json, vbpir, sealpir, env, best, selected_candidate, candidate_results) -> bool:
    """Validate the selected backend on the full schedule before publishing it."""
    final_json = folder / "formal.json"
    final_log = folder / "formal.log"
    final_process = folder / "formal-process.json"
    formal_process = runner(
        command_for_candidate(selected_candidate, vbpir=vbpir, sealpir=sealpir, height=height, leaf_file=leaf_file,
                              index_json=index_json, output=final_json, warmups=args.warmups, trials=args.trials),
        log_path=final_log,
        output_path=final_json,
        env=env,
    )
    if not process_ok(formal_process):
        candidate_results.append(CandidateResult(f"formal:{best.label}", False, None, f"process exit {formal_process.get('exit_code')}"))
        print(f"{scale}: formal selected run failed for {best.label}", file=sys.stderr)
        return False
    if not final_process.exists() or not final_log.exists():
        raise TuningError(f"{scale}: formal run did not produce process and log artifacts")
    selected = load_json(final_json)
    formal_result = validate_candidate_result(selected_candidate, selected, height=height, trials=args.trials,
                                              warmups=args.warmups, leaves=leaves)
    formal_result.raise_if_ineligible(final_json)
    selected["backend_label"] = best.label
    selected["backend_family"] = selected_candidate.backend
    selected["candidate_eligibility"] = [result.as_dict() for result in candidate_results]
    selected["comparison_regime"] = {
        "scope": "TreePIR backend tuning only for the same generated leaf schedule and configured height",
        "formal_trials": args.trials,
        "warmups": args.warmups,
        "pilot_trials": args.pilot_trials,
        "global_optimality_claim": "none",
        "excluded_backends": ["Spiral", "PIRANA"],
        "note": "Spiral and PIRANA are separately investigated and not measured by this runner.",
    }
    final_json.write_text(json.dumps(selected, indent=2) + "\n")
    (tuning_dir / "selection.json").write_text(json.dumps({
        "height": height,
        "scale": scale,
        "selected": best.as_dict(),
        "candidate_eligibility": [result.as_dict() for result in candidate_results],
        "comparison_regime": selected["comparison_regime"],
    }, indent=2) + "\n")
    print(f"{scale}: selected {best.label} mean_server_ms={best.mean_server_ms:.3f}")
    return True


def prepare_leaf_schedule(args, height, runner, *, folder, tuning_dir, scale,
                          leaf_count, onion, vbpir, env):
    """Generate shared leaves and validate their VBPIR index mapping before pilots."""
    schedule_json = tuning_dir / "schedule-treepir-tuned.json"
    schedule_process = runner(
        [str(onion), "--test", "tree_superrecord_bench", "--leaf-count", str(leaf_count),
         "--experiments", str(args.trials), "--warmup", str(args.warmups), "--trial-seed", str(args.trial_seed)],
        log_path=schedule_json.with_suffix(".log"),
        output_path=schedule_json,
        env={**env, "SUPERRECORD_PLAN_ONLY": "1", "SUPERRECORD_SKIP_LEGACY": "1",
             "SUPERRECORD_BUNDLE_ARITIES": "1,3", "SUPERRECORD_MAX_ARITY": "3",
             "SUPERRECORD_OUTPUT_JSON": str(schedule_json)},
    )
    if not process_ok(schedule_process):
        print(f"{scale}: schedule generation failed", file=sys.stderr)
        return None
    leaves = validate_leaf_schedule(schedule_json, trials=args.trials, warmups=args.warmups, leaf_count=leaf_count)
    leaf_file = folder / "treepir-leaves-tuned.txt"
    write_leaf_file(leaf_file, leaves)

    index_json = tuning_dir / "vbpir-index-only.json"
    index_process = runner(
        [str(vbpir), "--height", str(height), "--leaf-file", str(leaf_file), "--warmup", str(args.warmups),
         "--trials", str(args.trials), "--output", str(index_json), "--validate-index-only"],
        log_path=index_json.with_suffix(".log"),
        output_path=index_json,
        env=env,
    )
    if not process_ok(index_process):
        print(f"{scale}: VBPIR index-only run failed", file=sys.stderr)
        return None
    validate_index_only(index_json, height=height, trials=args.trials, warmups=args.warmups, leaves=leaves)

    return leaves, leaf_file, index_json


def run_one_height(args: argparse.Namespace, height: int, runner: Callable[..., dict[str, Any]]) -> bool:
    scale = scale_for_height(height)
    leaf_count = 1 << height
    run_root = args.run_root
    source = run_root / "source"
    folder = run_root / "outputs" / "treepir-backend-tuning" / scale
    tuning_dir = folder / "candidates"
    folder.mkdir(parents=True, exist_ok=True)
    tuning_dir.mkdir(parents=True, exist_ok=True)

    env = os.environ.copy()
    env["RUN_ROOT"] = str(run_root)
    env["TREEPIR_DATA_DIR"] = str(run_root / "deps" / "TreePIR" / "VBPIR_TreePIR")

    vbpir = args.vbpir_bin or run_root / "deps" / "TreePIR" / "VBPIR_TreePIR" / "build" / "bin" / "vbpir_treepir"
    sealpir = args.sealpir_binary or source / "build-treepir-sealpir" / "treepir_sealpir_adapter"
    onion = args.onion_bin or source / "build-native" / "Onion-PIR"

    prepared = prepare_leaf_schedule(
        args, height, runner, folder=folder, tuning_dir=tuning_dir, scale=scale,
        leaf_count=leaf_count, onion=onion, vbpir=vbpir, env=env,
    )
    if prepared is None:
        return False
    leaves, leaf_file, index_json = prepared

    candidate_by_label = {candidate.label: candidate for candidate in candidates_for_height(height)}
    candidate_results = run_pilot_candidates(
        args, height, runner, tuning_dir=tuning_dir, leaves=leaves, leaf_file=leaf_file,
        index_json=index_json, vbpir=vbpir, sealpir=sealpir, env=env,
    )

    best = select_best_candidate(candidate_results)
    selected_candidate = candidate_by_label[best.label]
    return run_formal_candidate(
        args, height, runner, folder=folder, tuning_dir=tuning_dir, scale=scale,
        leaves=leaves, leaf_file=leaf_file, index_json=index_json, vbpir=vbpir,
        sealpir=sealpir, env=env, best=best, selected_candidate=selected_candidate,
        candidate_results=candidate_results,
    )


def parse_args(argv: list[str] | None = None) -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--run-root", required=True, type=Path)
    parser.add_argument("--heights", nargs="+", type=int, default=[24, 26])
    parser.add_argument("--trials", type=int, default=16)
    parser.add_argument("--warmups", type=int, default=3)
    parser.add_argument("--pilot-trials", type=int, default=16)
    parser.add_argument("--trial-seed", type=int, default=7)
    parser.add_argument("--vbpir-bin", type=Path)
    parser.add_argument("--sealpir-binary", type=Path)
    parser.add_argument("--onion-bin", type=Path)
    args = parser.parse_args(argv)
    if args.trials <= 0 or args.warmups < 0 or args.pilot_trials <= 0:
        parser.error("--trials and --pilot-trials must be positive; --warmups must be nonnegative")
    for height in args.heights:
        scale_for_height(height)
    return args


def main(argv: list[str] | None = None, *, runner: Callable[..., dict[str, Any]] = run_process) -> int:
    try:
        args = parse_args(argv)
        ok = True
        for height in args.heights:
            ok = run_one_height(args, height, runner) and ok
        return 0 if ok else 1
    except TuningError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
