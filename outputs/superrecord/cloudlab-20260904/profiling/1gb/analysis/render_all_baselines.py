#!/usr/bin/env python3
"""Render compact CloudLab 1GiB baseline comparison report and figure."""

from __future__ import annotations

import argparse
import json
import math
import os
import statistics
from pathlib import Path
from typing import Any


BASE_DIR = Path(__file__).resolve().parents[1]
ANALYSIS_DIR = Path(__file__).resolve().parent
EXPECTED_LEAF_COUNT = 1 << 24


class ReportError(Exception):
    pass


def load_json(path: Path) -> dict[str, Any]:
    try:
        data = json.loads(path.read_text())
    except FileNotFoundError as exc:
        raise ReportError(f"{path}: missing") from exc
    except json.JSONDecodeError as exc:
        raise ReportError(f"{path}: invalid JSON: {exc}") from exc
    if not isinstance(data, dict):
        raise ReportError(f"{path}: JSON is not an object")
    return data


def require_process_ok(name: str) -> None:
    process = load_json(BASE_DIR / f"{name}-process.json")
    if process.get("exit_code") != 0:
        raise ReportError(f"{name}-process.json: exit_code is not 0")


def stats(values: list[float]) -> dict[str, float | int]:
    if not values:
        raise ReportError("empty sample vector")
    ordered = sorted(values)
    return {
        "n": len(values),
        "mean": statistics.fmean(values),
        "median": statistics.median(values),
        "p95": ordered[math.ceil(0.95 * len(ordered)) - 1],
        "sd": statistics.stdev(values) if len(values) > 1 else 0.0,
    }


def mean_numeric(values: list[Any], label: str) -> float:
    if not values:
        raise ReportError(f"{label}: no values")
    out = []
    for value in values:
        if not isinstance(value, (int, float)) or isinstance(value, bool):
            raise ReportError(f"{label}: non-numeric value")
        out.append(float(value))
    return statistics.fmean(out)


def hbytes(value: int | float) -> str:
    units = ("B", "KiB", "MiB", "GiB")
    amount = float(value)
    for unit in units:
        if amount < 1024.0 or unit == units[-1]:
            return f"{amount:.2f} {unit}" if unit != "B" else f"{int(value)} B"
        amount /= 1024.0
    return f"{int(value)} B"


def validate_leaf_count(name: str, data: dict[str, Any]) -> None:
    leaf = data.get("leaf_count")
    if leaf is None and isinstance(data.get("workload"), dict):
        leaf = data["workload"].get("leaf_count")
    if leaf != EXPECTED_LEAF_COUNT:
        raise ReportError(f"{name}: leaf_count {leaf} != {EXPECTED_LEAF_COUNT}")


def merkle_row(name: str, case_name: str, label: str, rank_scope: str) -> dict[str, Any]:
    require_process_ok(name)
    data = load_json(BASE_DIR / f"{name}.json")
    validate_leaf_count(name, data)
    workload = data.get("workload", {})
    cases = data.get("cases")
    if not isinstance(cases, list):
        raise ReportError(f"{name}: missing cases")
    case = next((item for item in cases if item.get("name") == case_name), None)
    if not isinstance(case, dict):
        raise ReportError(f"{name}: missing case {case_name}")
    if case.get("correctness_passed") is not True:
        raise ReportError(f"{name}/{case_name}: correctness failed")
    server = [float(v) for v in case.get("server_compute_samples_ms", [])]
    expected = int(workload.get("measured_trials", len(server)))
    if len(server) != expected:
        raise ReportError(f"{name}/{case_name}: server samples {len(server)} != {expected}")
    return {
        "label": label,
        "rank_scope": rank_scope,
        "samples": len(server),
        "time": stats(server),
        "query_bytes": int(case["online_query_bytes_modeled"]),
        "query_kind": "modeled",
        "response_bytes": int(case["online_response_bytes_actual"]),
        "response_kind": "actual",
        "helper_bytes": int(case["helper_key_bytes_modeled"]),
        "helper_kind": "modeled",
        "storage_bytes": int(case.get("physical_preprocessed_storage_bytes") or 0),
        "scope_note": case.get("layer_layout_policy") or "",
    }


def sampled_row(name: str, label: str, rank_scope: str) -> dict[str, Any]:
    require_process_ok(name)
    data = load_json(BASE_DIR / f"{name}.json")
    validate_leaf_count(name, data)
    samples = data.get("samples")
    if not isinstance(samples, list):
        raise ReportError(f"{name}: missing samples")
    expected = int(data.get("measured_trials", len(samples)))
    if len(samples) != expected:
        raise ReportError(f"{name}: samples {len(samples)} != {expected}")
    if not all(sample.get("correct") is True for sample in samples):
        raise ReportError(f"{name}: at least one sample failed correctness")
    comm = data.get("communication", {})
    q = comm.get("modeled_query_bytes", comm.get("modeled_query_bytes_per_trial"))
    r = comm.get("actual_response_bytes", comm.get("actual_response_bytes_per_trial"))
    if q is None or r is None:
        raise ReportError(f"{name}: missing query/response byte fields")
    scope_note = ""
    if name in {"cuckoo", "cuckoo_tuned"}:
        bucket_count = int(data["num_buckets"])
        scope_note = f"{bucket_count} buckets, +1-bucket variant"
        if name == "cuckoo_tuned":
            layouts = data.get("bucket_layouts", [])
            if not layouts:
                raise ReportError("cuckoo_tuned: missing bucket_layouts")
            n0 = int(layouts[0]["layout"]["N0"])
            scope_note += f", h_q={int(data['requested_expansion_height'])} N0={n0}"
    return {
        "label": label,
        "rank_scope": rank_scope,
        "samples": len(samples),
        "time": stats([float(sample["server_total_ms"]) for sample in samples]),
        "query_bytes": int(q),
        "query_kind": "modeled",
        "response_bytes": int(r),
        "response_kind": "actual",
        "helper_bytes": int(comm.get("modeled_helper_key_bytes", 0)),
        "helper_kind": "modeled",
        "storage_bytes": int((data.get("storage") or {}).get("estimated_preprocessed_bytes") or 0),
        "scope_note": scope_note,
    }


def treepir_row() -> dict[str, Any]:
    name = "treepir"
    require_process_ok(name)
    data = load_json(BASE_DIR / "treepir.json")
    validate_leaf_count(name, data)
    measured = [row for row in data.get("results", []) if row.get("phase") == "measured"]
    if len(measured) != 100:
        raise ReportError(f"treepir: measured rows {len(measured)} != 100")
    if not all(row.get("correct") is True for row in measured):
        raise ReportError("treepir: at least one measured row failed correctness")
    return {
        "label": "TreePIR",
        "rank_scope": "ranked",
        "samples": len(measured),
        "time": stats([float(row["server_ms"]) for row in measured]),
        "query_bytes": int(mean_numeric([row.get("actual_query_payload_bytes_unseeded") for row in measured], "treepir actual query")),
        "query_kind": "actual unseeded",
        "response_bytes": int(mean_numeric([row.get("actual_response_payload_bytes") for row in measured], "treepir actual response")),
        "response_kind": "actual",
        "helper_bytes": int(data.get("actual_helper_key_bytes_uncompressed", 0)),
        "helper_kind": "actual uncompressed",
        "storage_bytes": 0,
        "scope_note": "SEAL BFV N=8192, coeff bits 42/58/58/60, plain bits 28",
    }


def generic_rows() -> tuple[dict[str, Any], dict[str, Any], dict[str, Any]]:
    require_process_ok("generic")
    raw = load_json(BASE_DIR / "generic.json")
    validate_leaf_count("generic", raw)
    analysis = load_json(ANALYSIS_DIR / "generic-analysis.json")
    raw_by_label = {c["label"]: c for c in raw["candidates"]}
    analysis_by_label = {c["label"]: c for c in analysis["candidates"]}
    for candidate in raw["candidates"]:
        if candidate.get("eligible") is not True:
            raise ReportError(f"generic/{candidate.get('label')}: not eligible")
        if len(candidate.get("samples", [])) != 100:
            raise ReportError(f"generic/{candidate.get('label')}: samples != 100")
    def build(label: str, row_label: str) -> dict[str, Any]:
        source = raw_by_label[label]
        analyzed = analysis_by_label[label]
        wire = source["wire"]
        shape = source["shape"]
        raw_stats = stats([float(sample["server_total_ms"]) for sample in source["samples"]])
        return {
            "label": row_label,
            "rank_scope": "ranked",
            "samples": int(raw_stats["n"]),
            "time": raw_stats,
            "query_bytes": int(wire["modeled_seeded_query_bytes"]),
            "query_kind": "modeled",
            "response_bytes": int(wire["actual_response_ciphertext_payload_bytes"]),
            "response_kind": "actual",
            "helper_bytes": int(source["candidate_metrics"]["modeled_per_client_eval_key_bytes"]),
            "helper_kind": "modeled",
            "storage_bytes": int(source["memory"]["database_bytes"]),
            "steady_state_bytes": int(analyzed["steady_state_bytes"]),
            "scope_note": f"h={shape['bundle_arity']} G={shape['record_coeffs']} P={shape['plane_count']} N0={shape['N0']} b={shape['beta_bits']}",
        }
    comparison = analysis["comparisons"]["bundle3"]["generic-h1-mandatory-1"]
    return build("bundle3", "Generic best h3"), build("generic-h1-mandatory-1", "Generic fastest h1"), comparison


def collect_rows() -> tuple[list[dict[str, Any]], list[dict[str, Any]], dict[str, Any]]:
    generic_best, generic_h1, generic_comparison = generic_rows()
    ranked = [
        generic_best,
        generic_h1,
        sampled_row("tree_g32", "Tree g=32", "ranked"),
        sampled_row("tree_compressed", "Tree compressed", "ranked"),
        merkle_row("layerwise", "merkle_layerwise", "Layerwise legacy", "ranked"),
        merkle_row("layerwise_tuned", "merkle_layerwise", "Layerwise tuned", "ranked"),
        sampled_row("cuckoo", "Cuckoo legacy", "ranked"),
        sampled_row("cuckoo_tuned", "Cuckoo tuned", "ranked"),
        treepir_row(),
        merkle_row("flat", "merkle_flat", "Flat", "ranked"),
    ]
    references = [
        merkle_row("standard", "standard_onionpir", "Standard OnionPIR", "reference"),
        sampled_row("tree_g1", "Tree g=1 12-bit diagnostic", "reference"),
    ]
    return ranked, references, generic_comparison


def md_table(headers: list[str], rows: list[list[str]]) -> str:
    return "\n".join(
        ["| " + " | ".join(headers) + " |",
         "| " + " | ".join("---" for _ in headers) + " |"]
        + ["| " + " | ".join(row) + " |" for row in rows]
    )


def render_markdown(ranked: list[dict[str, Any]], references: list[dict[str, Any]], generic_comparison: dict[str, Any]) -> None:
    ranked_sorted = sorted(ranked, key=lambda row: row["time"]["mean"])
    rows = []
    for idx, row in enumerate(ranked_sorted, 1):
        rows.append([
            str(idx),
            row["label"],
            str(row["samples"]),
            f"{row['time']['mean']:.3f}",
            f"{row['time']['median']:.3f}",
            f"{row['time']['p95']:.3f}",
            f"{row['time']['sd']:.3f}",
            f"{row['query_bytes']} {row['query_kind']}",
            f"{row['response_bytes']} {row['response_kind']}",
            row["scope_note"],
        ])
    for row in references:
        rows.append([
            "ref",
            row["label"],
            str(row["samples"]),
            f"{row['time']['mean']:.3f}",
            f"{row['time']['median']:.3f}",
            f"{row['time']['p95']:.3f}",
            f"{row['time']['sd']:.3f}",
            f"{row['query_bytes']} {row['query_kind']}",
            f"{row['response_bytes']} {row['response_kind']}",
            row["scope_note"],
        ])

    helper_rows = []
    for row in ranked_sorted + references:
        helper_rows.append([
            row["label"],
            str(row["helper_bytes"]),
            row["helper_kind"],
            hbytes(row["helper_bytes"]),
        ])

    storage_rows = []
    for row in ranked_sorted + references:
        if row.get("storage_bytes") or row.get("steady_state_bytes"):
            storage_rows.append([
                row["label"],
                str(row.get("storage_bytes") or ""),
                hbytes(row["storage_bytes"]) if row.get("storage_bytes") else "",
                str(row.get("steady_state_bytes") or ""),
                hbytes(row["steady_state_bytes"]) if row.get("steady_state_bytes") else "",
            ])

    best = ranked_sorted[0]
    lines = [
        "# CloudLab 1GiB All Baselines Comparison",
        "",
        f"Fastest ranked complete-path configuration: {best['label']} at {best['time']['mean']:.3f} ms mean server time over {best['samples']} samples.",
        f"Generic best h3 versus fastest h1: speedup {generic_comparison['speedup']:.6f}, paired 95% CI {generic_comparison['speedup_ci95'][0]:.6f}..{generic_comparison['speedup_ci95'][1]:.6f}, time reduction {generic_comparison['server_time_reduction_percent']:.5f}%.",
        "",
        "## Main Table",
        "",
        md_table(["rank", "scheme", "samples", "mean ms", "median ms", "p95 ms", "sample SD ms", "query bytes", "response bytes", "config note"], rows),
        "",
        "## Per-Query Communication",
        "",
        "The figure reports query and actual response bytes per complete path. Query bytes are modeled except TreePIR, where the actual unseeded query payload is used. Helper/eval keys are setup material and appear only in the helper-key table.",
        "",
        "![All baselines comparison](all-baselines-comparison.png)",
        "",
        "## Helper / Eval Keys",
        "",
        md_table(["scheme", "once-only bytes", "basis", "human"], helper_rows),
        "",
        "Cuckoo legacy and tuned have the same online query/response bytes in this run, but setup keys differ: legacy is 1,130,880 B and tuned is 1,488,000 B, an increase of 357,120 B from the reference-parameter alignment.",
        "",
        "## Storage",
        "",
        md_table(["scheme", "DB bytes", "human DB", "estimated steady-state bytes", "human steady-state"], storage_rows),
        "",
        "## Scope And Limits",
        "",
        "Results are configuration measurements for the recorded CloudLab 1GiB run. Generic super-record rows retrieve the full ancestor path including the root; the non-root baselines follow their recorded benchmark configurations. TreePIR uses different SEAL BFV parameters from the OnionPIRv2-family rows, listed in its config note. These measurements support this configuration comparison, not a universal algorithmic speedup claim.",
        "",
        "No bootstrap was rerun for this report. The only CI shown is read from `generic-analysis.json`; all other comparisons are descriptive.",
    ]
    (ANALYSIS_DIR / "ALL_BASELINES_COMPARISON.md").write_text("\n".join(lines) + "\n")


def render_plot(ranked: list[dict[str, Any]]) -> None:
    import matplotlib.pyplot as plt
    import numpy as np

    rows = sorted(ranked, key=lambda row: row["time"]["mean"])
    labels = [row["label"] for row in rows]
    times = np.array([row["time"]["mean"] / 1000.0 for row in rows])
    query_kib = np.array([row["query_bytes"] / 1024.0 for row in rows])
    response_kib = np.array([row["response_bytes"] / 1024.0 for row in rows])
    y = np.arange(len(rows))

    fig, (ax_time, ax_comm) = plt.subplots(1, 2, figsize=(13.8, 7.3))
    ax_time.barh(y, times, color="#4C78A8")
    ax_time.set_yticks(y, labels)
    ax_time.invert_yaxis()
    ax_time.set_xlabel("mean server seconds")
    ax_time.set_title("Complete-path server time")
    ax_time.set_xlim(0.0, max(times) * 1.22)
    ax_time.grid(axis="x", color="#dddddd", linewidth=0.8)
    ax_time.set_axisbelow(True)
    for yi, value, samples in zip(y, times, [row["samples"] for row in rows]):
        ax_time.text(value + max(times) * 0.01, yi, f"{value:.2f}s n={samples}", va="center", fontsize=8)

    ax_comm.barh(y, query_kib, color="#F58518", label="query")
    ax_comm.barh(y, response_kib, left=query_kib, color="#54A24B", label="response (actual)")
    ax_comm.set_yticks(y, [])
    ax_comm.invert_yaxis()
    ax_comm.set_xlabel("KiB per complete path")
    ax_comm.set_title("Per-query communication")
    comm_totals = query_kib + response_kib
    comm_xlim = max(comm_totals) * 1.36
    ax_comm.set_xlim(0.0, comm_xlim)
    ax_comm.grid(axis="x", color="#dddddd", linewidth=0.8)
    ax_comm.set_axisbelow(True)
    ax_comm.legend(loc="lower right", frameon=False)
    for yi, total, qkind in zip(y, comm_totals, [row["query_kind"] for row in rows]):
        label = "query actual\n(unseeded)" if qkind == "actual unseeded" else "query modeled"
        xpos = total + max(comm_totals) * 0.012
        ax_comm.text(xpos, yi, label, va="center", ha="left", fontsize=8)

    fig.suptitle("CloudLab 1GiB all complete-path baselines")
    fig.text(
        0.5,
        0.045,
        "Scope: generic/tree include root; other baselines use non-root path; TreePIR uses different HE parameters.",
        ha="center",
        fontsize=9,
    )
    fig.subplots_adjust(left=0.18, right=0.97, top=0.88, bottom=0.20, wspace=0.09)
    fig.savefig(ANALYSIS_DIR / "all-baselines-comparison.png", dpi=180)
    fig.savefig(ANALYSIS_DIR / "all-baselines-comparison.pdf")
    plt.close(fig)


def main() -> int:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.parse_args()
    os.environ.setdefault("MPLCONFIGDIR", "/private/tmp/all-baselines-mpl")
    ranked, references, generic_comparison = collect_rows()
    render_markdown(ranked, references, generic_comparison)
    render_plot(ranked)
    print(ANALYSIS_DIR / "ALL_BASELINES_COMPARISON.md")
    print(ANALYSIS_DIR / "all-baselines-comparison.png")
    print(ANALYSIS_DIR / "all-baselines-comparison.pdf")
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
