#!/usr/bin/env python3
"""Validate and summarize CloudLab profile JSON outputs."""

from __future__ import annotations

import argparse
import csv
import json
import math
import statistics
import sys
from collections import defaultdict
from pathlib import Path
from typing import Any


Row = dict[str, Any]
SUPPORTED_SCALES = ("256mb", "512mb", "1gb", "4gb")


class ProfileError(Exception):
    pass


def load_json(path: Path) -> Any:
    try:
        return json.loads(path.read_text())
    except json.JSONDecodeError as exc:
        raise ProfileError(f"{path}: invalid JSON: {exc}") from exc


def finite_ms(value: Any, path: Path, name: str) -> float:
    if not isinstance(value, (int, float)) or isinstance(value, bool):
        raise ProfileError(f"{path}: {name} is not numeric")
    value = float(value)
    if not math.isfinite(value) or value < 0.0:
        raise ProfileError(f"{path}: {name} is not a finite non-negative timing")
    return value


def finite_metric(value: Any, path: Path, name: str) -> float:
    if not isinstance(value, (int, float)) or isinstance(value, bool):
        raise ProfileError(f"{path}: {name} is not numeric")
    value = float(value)
    if not math.isfinite(value) or value < 0.0:
        raise ProfileError(f"{path}: {name} is not a finite non-negative value")
    return value


def require_count(actual: int, expected: int | None, path: Path, what: str) -> None:
    if expected is not None and actual != expected:
        raise ProfileError(f"{path}: {what} count {actual} != expected {expected}")


def require_leaf_count(data: dict[str, Any], path: Path, expected: int | None) -> None:
    if expected is None:
        return
    actual = data.get("leaf_count")
    if actual is None and isinstance(data.get("workload"), dict):
        actual = data["workload"].get("leaf_count")
    if actual is None and isinstance(data.get("shape"), dict):
        actual = data["shape"].get("leaf_count")
    if actual != expected:
        raise ProfileError(f"{path}: leaf_count {actual} != expected {expected}")


def process_peak_rss(path: Path) -> int | None:
    process_path = path.with_name(f"{path.stem}-process.json")
    if not process_path.exists():
        return None
    data = load_json(process_path)
    value = data.get("process_peak_rss_bytes")
    if isinstance(value, int) and value >= 0:
        return value
    return None


def case_name_from_path(path: Path) -> str:
    return path.stem


def scale_from_path(path: Path, root: Path | None = None) -> str:
    if root is not None:
        try:
            rel = path.relative_to(root)
            if rel.parts and rel.parts[0] in SUPPORTED_SCALES:
                return rel.parts[0]
        except ValueError:
            pass
    for part in path.parts:
        if part in SUPPORTED_SCALES:
            return part
    return ""


def add_value(rows: list[Row], path: Path, scale: str, case: str, family: str, variant: str,
              sample_index: int | str, stage: str, value: Any, kind: str = "primary",
              peak_rss_bytes: int | None = None, unit: str = "ms", basis: str = "actual") -> None:
    rows.append({
        "scale": scale,
        "case": case,
        "family": family,
        "variant": variant,
        "sample_index": sample_index,
        "stage": stage,
        "kind": kind,
        "value": finite_ms(value, path, stage) if unit == "ms" else finite_metric(value, path, stage),
        "unit": unit,
        "basis": basis,
        "source_file": str(path),
        "peak_rss_bytes": peak_rss_bytes,
    })


def add_bytes(rows: list[Row], path: Path, scale: str, case: str, family: str, variant: str,
              sample_index: int | str, stage: str, value: Any, kind: str = "communication",
              peak_rss_bytes: int | None = None, basis: str = "actual") -> None:
    if value is None:
        return
    add_value(rows, path, scale, case, family, variant, sample_index, stage, value, kind, peak_rss_bytes, "bytes", basis)


def infer_family(data: Any) -> str | None:
    if not isinstance(data, dict):
        return None
    if isinstance(data.get("cases"), list):
        return "merkle"
    if data.get("model") == "SEAL+VBPIRTreePIR" or "measured_summary" in data and "results" in data:
        return "treepir"
    if isinstance(data.get("samples"), list):
        return "sampled"
    return None


def expected_trials(data: dict[str, Any], requested: int | None) -> int | None:
    if requested is not None:
        return requested
    if isinstance(data.get("measured_trials"), int):
        return data["measured_trials"]
    if isinstance(data.get("workload"), dict) and isinstance(data["workload"].get("measured_trials"), int):
        return data["workload"]["measured_trials"]
    if isinstance(data.get("trials"), int):
        return data["trials"]
    return None


# Profile validation


def validate_merkle(data: dict[str, Any], path: Path, trials: int | None, leaf_count: int | None) -> None:
    workload = data.get("workload", {})
    if leaf_count is not None and workload.get("leaf_count") != leaf_count:
        raise ProfileError(f"{path}: workload.leaf_count {workload.get('leaf_count')} != expected {leaf_count}")
    expected = expected_trials(data, trials)
    if not data.get("cases"):
        raise ProfileError(f"{path}: no benchmark cases")
    for case in data.get("cases", []):
        if not case.get("correctness_passed"):
            raise ProfileError(f"{path}: case {case.get('name')} failed correctness")
        samples = case.get("pipeline_profile_samples_ms")
        if not isinstance(samples, list):
            raise ProfileError(f"{path}: case {case.get('name')} missing pipeline_profile_samples_ms")
        require_count(len(samples), expected, path, f"case {case.get('name')} pipeline samples")
        for idx, sample in enumerate(samples):
            stages = sample.get("stages")
            if not isinstance(stages, dict) or not stages:
                raise ProfileError(f"{path}: case {case.get('name')} sample {idx} missing stages")
            for stage, value in stages.items():
                finite_ms(value, path, f"{case.get('name')}.{stage}")
        for key in ("server_compute_samples_ms", "client_query_samples_ms",
                    "response_serialize_samples_ms", "response_load_decrypt_extract_samples_ms"):
            values = case.get(key)
            if not isinstance(values, list):
                raise ProfileError(f"{path}: case {case.get('name')} missing {key}")
            require_count(len(values), expected, path, f"case {case.get('name')} {key}")
            for idx, value in enumerate(values):
                finite_ms(value, path, f"{case.get('name')}.{key}[{idx}]")


def validate_sampled(data: dict[str, Any], path: Path, trials: int | None, leaf_count: int | None) -> None:
    require_leaf_count(data, path, leaf_count)
    samples = data.get("samples")
    if not isinstance(samples, list):
        raise ProfileError(f"{path}: missing samples")
    require_count(len(samples), expected_trials(data, trials), path, "samples")
    for idx, sample in enumerate(samples):
        if sample.get("correct") is not True:
            raise ProfileError(f"{path}: sample {idx} failed correctness")
        for key in ("client_query_ms", "server_total_ms", "response_serialize_ms", "client_decode_ms"):
            finite_ms(sample.get(key), path, f"sample[{idx}].{key}")
        if sample.get("actual_response_bytes") is not None:
            finite_metric(sample["actual_response_bytes"], path, f"sample[{idx}].actual_response_bytes")
        stages = sample.get("stages_ms")
        if not isinstance(stages, dict) or not stages:
            raise ProfileError(f"{path}: sample {idx} missing stages_ms")
        for stage, value in stages.items():
            finite_ms(value, path, f"sample[{idx}].stages_ms.{stage}")
        diagnostics = sample.get("diagnostics_ms", {})
        if diagnostics is not None:
            if not isinstance(diagnostics, dict):
                raise ProfileError(f"{path}: sample {idx} diagnostics_ms is not an object")
            for stage, value in diagnostics.items():
                finite_ms(value, path, f"sample[{idx}].diagnostics_ms.{stage}")
    communication = data.get("communication", {})
    if communication is not None:
        if not isinstance(communication, dict):
            raise ProfileError(f"{path}: communication is not an object")
        for key, value in communication.items():
            if not key.endswith("_kind"):
                finite_metric(value, path, f"communication.{key}")


def validate_treepir(data: dict[str, Any], path: Path, trials: int | None, leaf_count: int | None) -> None:
    require_leaf_count(data, path, leaf_count)
    results = data.get("results")
    if not isinstance(results, list):
        raise ProfileError(f"{path}: missing results")
    measured = [r for r in results if r.get("phase") == "measured"]
    require_count(len(measured), expected_trials(data, trials), path, "measured results")
    summary = data.get("measured_summary", {})
    if isinstance(summary, dict):
        if summary.get("all_correct") is not True:
            raise ProfileError(f"{path}: measured_summary.all_correct is not true")
        if isinstance(summary.get("count"), int):
            require_count(summary["count"], len(measured), path, "measured_summary")
    for idx, result in enumerate(measured):
        if result.get("correct") is not True:
            raise ProfileError(f"{path}: measured result {idx} failed correctness")
        for key in ("query_ms", "server_ms", "decode_ms"):
            finite_ms(result.get(key), path, f"measured[{idx}].{key}")
        for key in ("index_ms", "server_first_dimension_ms", "server_second_dimension_ms",
                    "server_last_dimension_ms", "server_merge_ms", "response_serialize_ms"):
            if result.get(key) is not None:
                finite_ms(result.get(key), path, f"measured[{idx}].{key}")
        for key in ("query_payload_bytes_bound_save_size_seeded_div2",
                    "response_payload_bytes_bound_save_size",
                    "actual_query_payload_bytes_unseeded",
                    "actual_response_payload_bytes"):
            if result.get(key) is not None:
                finite_metric(result.get(key), path, f"measured[{idx}].{key}")
        if result.get("query_serialize_ms") is not None:
            finite_ms(result["query_serialize_ms"], path, f"measured[{idx}].query_serialize_ms")
    for key in ("actual_helper_key_bytes_uncompressed", "offline_setup_ms"):
        if data.get(key) is not None:
            finite_metric(data[key], path, key)


def validate_profile(data: dict[str, Any], path: Path, trials: int | None, leaf_count: int | None) -> str:
    family = infer_family(data)
    if family == "merkle":
        validate_merkle(data, path, trials, leaf_count)
    elif family == "treepir":
        validate_treepir(data, path, trials, leaf_count)
    elif family == "sampled":
        validate_sampled(data, path, trials, leaf_count)
    else:
        raise ProfileError(f"{path}: unrecognized profile schema")
    return family


# Validated profiles to individual sample rows


def collect_merkle(data: dict[str, Any], path: Path, scale: str, peak: int | None) -> list[Row]:
    rows: list[Row] = []
    for case in data["cases"]:
        variant = str(case.get("name", case_name_from_path(path)))
        policy = case.get("layer_layout_policy")
        if policy and policy != "legacy":
            variant += "_" + str(policy)
        for idx, sample in enumerate(case["pipeline_profile_samples_ms"]):
            for stage, value in sample["stages"].items():
                add_value(rows, path, scale, variant, "merkle", variant, idx, stage, value, "primary", peak)
        sample_keys = {
            "server_compute_ms": "server_compute_samples_ms",
            "client_query_ms": "client_query_samples_ms",
            "response_serialize_ms": "response_serialize_samples_ms",
            "response_load_decrypt_extract_ms": "response_load_decrypt_extract_samples_ms",
        }
        for stage, key in sample_keys.items():
            for idx, value in enumerate(case.get(key, [])):
                add_value(rows, path, scale, variant, "merkle", variant, idx, stage, value, "primary", peak)
        add_value(rows, path, scale, variant, "merkle", variant, "setup", "setup_ms", case.get("setup_ms"), "offline", peak)
        for stage, key, basis in (
            ("query_upload_bytes", "online_query_bytes_modeled", "modeled"),
            ("response_download_bytes", "online_response_bytes_actual", "actual"),
            ("helper_key_bytes", "helper_key_bytes_modeled", "modeled"),
            ("first_session_total_bytes", "first_session_total_bytes_mixed", "mixed"),
            ("online_total_bytes", "online_total_bytes_mixed", "mixed"),
        ):
            add_bytes(rows, path, scale, variant, "merkle", variant, "communication", stage, case.get(key), basis=basis, peak_rss_bytes=peak)
    return rows


def collect_sampled(data: dict[str, Any], path: Path, scale: str, peak: int | None) -> list[Row]:
    rows: list[Row] = []
    case = case_name_from_path(path)
    family = str(data.get("scheme") or case)
    for idx, sample in enumerate(data["samples"]):
        for stage, value in sample["stages_ms"].items():
            add_value(rows, path, scale, case, family, "", idx, stage, value, "primary", peak)
        for stage, key in {
            "client_query_ms": "client_query_ms",
            "server_total_ms": "server_total_ms",
            "response_serialize_ms": "response_serialize_ms",
            "client_decode_ms": "client_decode_ms",
        }.items():
            add_value(rows, path, scale, case, family, "", idx, stage, sample[key], "primary", peak)
        for stage, value in sample.get("diagnostics_ms", {}).items():
            add_value(rows, path, scale, case, family, "", idx, f"diagnostic_{stage}", value, "diagnostic", peak)
    setup = data.get("offline_setup_ms", data.get("setup_ms"))
    if setup is not None:
        add_value(rows, path, scale, case, family, "", "setup", "offline_setup_ms", setup, "offline", peak)
    communication = data.get("communication", {})
    if isinstance(communication, dict):
        add_bytes(rows, path, scale, case, family, "", "communication", "query_upload_bytes", communication.get("modeled_query_bytes"), basis="modeled", peak_rss_bytes=peak)
        add_bytes(rows, path, scale, case, family, "", "communication", "query_upload_bytes", communication.get("modeled_query_bytes_per_trial"), basis="modeled", peak_rss_bytes=peak)
        add_bytes(rows, path, scale, case, family, "", "communication", "response_download_bytes", communication.get("actual_response_bytes"), basis="actual", peak_rss_bytes=peak)
        if not any(sample.get("actual_response_bytes") is not None for sample in data["samples"]):
            add_bytes(rows, path, scale, case, family, "", "communication", "response_download_bytes", communication.get("actual_response_bytes_per_trial"), basis="actual", peak_rss_bytes=peak)
        add_bytes(rows, path, scale, case, family, "", "communication", "helper_key_bytes", communication.get("modeled_helper_key_bytes"), basis="modeled", peak_rss_bytes=peak)
        add_bytes(rows, path, scale, case, family, "", "communication", "compression_key_bytes", communication.get("modeled_compression_key_bytes"), basis="modeled", peak_rss_bytes=peak)
        modeled_query = communication.get("modeled_query_bytes")
        if modeled_query is None:
            modeled_query = communication.get("modeled_query_bytes_per_trial")
        modeled_helper = communication.get("modeled_helper_key_bytes")
        actual_response = communication.get("actual_response_bytes", communication.get("actual_response_bytes_per_trial"))
        if modeled_query is not None and actual_response is not None:
            add_bytes(rows, path, scale, case, family, "", "communication", "online_total_bytes_mixed",
                      float(modeled_query) + float(actual_response), basis="mixed", peak_rss_bytes=peak)
        if modeled_query is not None and modeled_helper is not None:
            add_bytes(rows, path, scale, case, family, "", "communication", "first_session_modeled_query_plus_helper_bytes",
                      float(modeled_query) + float(modeled_helper), basis="modeled", peak_rss_bytes=peak)
        for idx, sample in enumerate(data["samples"]):
            add_bytes(rows, path, scale, case, family, "", idx, "response_download_bytes",
                      sample.get("actual_response_bytes"), basis="actual", peak_rss_bytes=peak)
    return rows


def collect_treepir(data: dict[str, Any], path: Path, scale: str, peak: int | None) -> list[Row]:
    rows: list[Row] = []
    variant = str(data.get("backend_label") or "")
    for idx, result in enumerate(r for r in data["results"] if r.get("phase") == "measured"):
        for stage, key in {
            "index_ms": "index_ms",
            "query_ms": "query_ms",
            "server_ms": "server_ms",
            "server_first_dimension_ms": "server_first_dimension_ms",
            "server_second_dimension_ms": "server_second_dimension_ms",
            "server_last_dimension_ms": "server_last_dimension_ms",
            "server_merge_ms": "server_merge_ms",
            "server_expansion_ms": "server_expansion_ms",
            "server_query_ntt_ms": "server_query_ntt_ms",
            "server_database_ntt_ms": "server_database_ntt_ms",
            "server_multiply_accumulate_ms": "server_multiply_accumulate_ms",
            "server_inverse_ntt_ms": "server_inverse_ntt_ms",
            "server_recursive_decomposition_modswitch_ms": "server_recursive_decomposition_modswitch_ms",
            "server_modswitch_ms": "server_modswitch_ms",
            "response_serialize_ms": "response_serialize_ms",
            "query_serialize_ms": "query_serialize_ms",
            "decode_ms": "decode_ms",
        }.items():
            if result.get(key) is not None:
                add_value(rows, path, scale, "treepir", "TreePIR", variant, idx, stage, result[key], "primary", peak)
        add_bytes(rows, path, scale, "treepir", "TreePIR", variant, idx, "query_upload_bytes",
                  result.get("query_payload_bytes_bound_save_size_seeded_div2"), basis="modeled_bound", peak_rss_bytes=peak)
        add_bytes(rows, path, scale, "treepir", "TreePIR", variant, idx, "query_upload_bytes",
                  result.get("actual_query_payload_bytes_unseeded"), basis="actual_unseeded", peak_rss_bytes=peak)
        response_key = "actual_response_payload_bytes" if result.get("actual_response_payload_bytes") is not None else "response_payload_bytes_bound_save_size"
        response_basis = "actual_uncompressed_save" if response_key == "actual_response_payload_bytes" else "save_size_bound"
        add_bytes(rows, path, scale, "treepir", "TreePIR", variant, idx, "response_download_bytes",
                  result.get(response_key), basis=response_basis, peak_rss_bytes=peak)
        if result.get("query_payload_bytes_bound_save_size_seeded_div2") is not None and result.get(response_key) is not None:
            add_bytes(rows, path, scale, "treepir", "TreePIR", variant, idx, "online_total_bytes_mixed",
                      float(result["query_payload_bytes_bound_save_size_seeded_div2"]) + float(result[response_key]),
                      basis=f"mixed:{response_basis}+modeled_bound", peak_rss_bytes=peak)
        if result.get("actual_query_payload_bytes_unseeded") is not None and response_key == "actual_response_payload_bytes":
            add_bytes(rows, path, scale, "treepir", "TreePIR", variant, idx, "online_total_bytes_actual_unseeded_query",
                      float(result["actual_query_payload_bytes_unseeded"]) + float(result[response_key]),
                      basis="actual_uncompressed_unseeded", peak_rss_bytes=peak)
    if data.get("index_prep_ms_excluded") is not None:
        add_value(rows, path, scale, "treepir", "TreePIR", variant, "setup", "index_prep_ms_excluded", data["index_prep_ms_excluded"], "offline", peak)
    if data.get("offline_setup_ms") is not None:
        add_value(rows, path, scale, "treepir", "TreePIR", variant, "setup", "offline_setup_ms", data["offline_setup_ms"], "offline", peak)
    for stage in ("offline_key_setup_ms", "database_setup_ms"):
        if data.get(stage) is not None:
            add_value(rows, path, scale, "treepir", "TreePIR", variant, "setup", stage, data[stage], "offline", peak)
    add_bytes(rows, path, scale, "treepir", "TreePIR", variant, "setup", "helper_key_bytes",
              data.get("actual_helper_key_bytes_uncompressed"), kind="communication_setup", basis="actual_uncompressed", peak_rss_bytes=peak)
    return rows


def collect_rows(path: Path, root: Path | None = None, trials: int | None = None) -> list[Row]:
    data = load_json(path)
    if data.get("validate_index_only") is True:
        return []
    family = infer_family(data)
    if family is None:
        return []
    validate_profile(data, path, trials, None)
    peak = process_peak_rss(path)
    scale = scale_from_path(path, root)
    if family == "merkle":
        return collect_merkle(data, path, scale, peak)
    if family == "treepir":
        return collect_treepir(data, path, scale, peak)
    if family == "sampled":
        return collect_sampled(data, path, scale, peak)
    return []


# Sample aggregation and report output


def summarize(values: list[float]) -> dict[str, float | int]:
    ordered = sorted(values)
    p95_index = max(0, math.ceil(0.95 * len(ordered)) - 1)
    return {
        "count": len(values),
        "mean": statistics.fmean(values),
        "median": statistics.median(values),
        "p95": ordered[p95_index],
        "stdev": statistics.stdev(values) if len(values) > 1 else 0.0,
    }


def write_sample_csv(root: Path, rows: list[Row]) -> None:
    csv_path = root / "stage_samples.csv"
    fieldnames = ["scale", "case", "family", "variant", "sample_index", "stage", "kind",
                  "value", "unit", "basis", "source_file", "peak_rss_bytes"]
    with csv_path.open("w", newline="") as handle:
        writer = csv.DictWriter(handle, fieldnames=fieldnames)
        writer.writeheader()
        writer.writerows(rows)


def summarize_rows(rows: list[Row]) -> list[Row]:
    """Group samples without combining distinct layouts, units, or bases."""
    grouped: dict[tuple[str, str, str, str, str, str, str], list[float]] = defaultdict(list)
    peaks: dict[tuple[str, str, str, str], list[int]] = defaultdict(list)
    for row in rows:
        grouped[(row["scale"], row["case"], row["family"], row["variant"], row["stage"], row["unit"], row["basis"])].append(row["value"])
        if row["peak_rss_bytes"] not in (None, ""):
            peaks[(row["scale"], row["case"], row["family"], row["variant"])].append(int(row["peak_rss_bytes"]))

    summaries = []
    for key, values in sorted(grouped.items()):
        scale, case, family, variant, stage, unit, basis = key
        entry = {
            "scale": scale,
            "case": case,
            "family": family,
            "variant": variant,
            "stage": stage,
            "unit": unit,
            "basis": basis,
            **summarize(values),
        }
        peak_key = (scale, case, family, variant)
        if peaks.get(peak_key):
            entry["peak_rss_bytes"] = max(peaks[peak_key])
        summaries.append(entry)

    return summaries


def write_summary_markdown(root: Path, source_rows: int, summaries: list[Row]) -> None:
    md_path = root / "PROFILE_SUMMARY.md"
    lines = [
        "# Profile Summary",
        "",
        f"- Source samples: {source_rows}",
        f"- Summary rows: {len(summaries)}",
        "",
        "| scale | case | variant | stage | unit | basis | count | mean | median | p95 | stdev | peak RSS bytes |",
        "| --- | --- | --- | --- | --- | --- | ---: | ---: | ---: | ---: | ---: | ---: |",
    ]
    for entry in summaries:
        if entry["stage"] not in {
            "server_total_ms",
            "server_compute_ms",
            "server_ms",
            "query_upload_bytes",
            "response_download_bytes",
            "online_total_bytes",
            "online_total_bytes_mixed",
            "helper_key_bytes",
        }:
            continue
        lines.append(
            f"| {entry['scale']} | {entry['case']} | {entry['variant']} | {entry['stage']} | "
            f"{entry['unit']} | {entry['basis']} | {entry['count']} | {entry['mean']:.3f} | "
            f"{entry['median']:.3f} | {entry['p95']:.3f} | {entry['stdev']:.3f} | "
            f"{entry.get('peak_rss_bytes', '')} |"
        )
    md_path.write_text("\n".join(lines) + "\n")


def write_outputs(root: Path, rows: list[Row]) -> None:
    write_sample_csv(root, rows)
    summaries = summarize_rows(rows)

    json_path = root / "profile_summary.json"
    json_path.write_text(json.dumps({
        "schema_version": 1,
        "source_rows": len(rows),
        "summaries": summaries,
    }, indent=2, sort_keys=True) + "\n")

    write_summary_markdown(root, len(rows), summaries)


def directory_files(root: Path) -> list[Path]:
    files: list[Path] = []
    for scale in SUPPORTED_SCALES:
        scale_dir = root / scale
        if scale_dir.exists():
            files.extend(sorted(scale_dir.glob("*.json")))
    if not files:
        files = sorted(path for path in root.glob("*.json") if path.name not in {"profile_summary.json"})
    return [path for path in files if not path.name.endswith("-process.json")
            and path.name not in {"schedule.json", "profile_summary.json"}]


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument("--validate", type=Path, help="validate one profile JSON")
    parser.add_argument("--directory", type=Path, help="summarize profile JSONs under ROOT/{256mb,512mb,1gb,4gb}")
    parser.add_argument("--trials", type=int, help="expected measured-trial count")
    parser.add_argument("--trials100", action="store_const", const=100, dest="trials",
                        help="expect 100 measured trials")
    parser.add_argument("--leaf-count", type=int, help="expected leaf count")
    parser.add_argument("--flat-trials", type=int, help="measured-trial count override for flat.json in a directory")
    args = parser.parse_args()
    if bool(args.validate) == bool(args.directory):
        parser.error("provide exactly one of --validate or --directory")
    return args


def collect_directory_rows(root: Path, trials: int | None, flat_trials: int | None) -> tuple[list[Row], list[Path]]:
    rows: list[Row] = []
    skipped: list[Path] = []
    for path in directory_files(root):
        try:
            file_trials = flat_trials if path.stem == "flat" and flat_trials is not None else trials
            rows.extend(collect_rows(path, root, file_trials))
        except ProfileError:
            if infer_family(load_json(path)) is None:
                skipped.append(path)
                continue
            raise
    if not rows:
        raise ProfileError(f"{root}: no profile samples found")
    return rows, skipped


def main() -> int:
    args = parse_args()
    try:
        if args.validate:
            data = load_json(args.validate)
            family = validate_profile(data, args.validate, args.trials, args.leaf_count)
            print(f"valid {family}: {args.validate}")
            return 0

        root = args.directory
        rows, skipped = collect_directory_rows(root, args.trials, args.flat_trials)
        write_outputs(root, rows)
        print(f"wrote {root / 'stage_samples.csv'}")
        print(f"wrote {root / 'profile_summary.json'}")
        print(f"wrote {root / 'PROFILE_SUMMARY.md'}")
        if skipped:
            print(f"skipped {len(skipped)} non-profile JSON files")
        return 0
    except ProfileError as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
