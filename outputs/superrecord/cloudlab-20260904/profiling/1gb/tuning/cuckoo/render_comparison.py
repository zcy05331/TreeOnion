#!/usr/bin/env python3
"""Render Cuckoo h7-h10 1 GiB pilot tuning report and plots."""

from __future__ import annotations

import argparse
import importlib.util
import json
import math
import os
import statistics
import sys
from pathlib import Path
from typing import Any


EXPECTED_HEIGHTS = (7, 8, 9, 10)
EXPECTED_TRIALS = 16
EXPECTED_WARMUPS = 3
EXPECTED_LEAF_COUNT = 1 << 24
STAGE_ORDER = (
    "expand",
    "convert",
    "first_dim_query_ntt",
    "first_dim_query_pack",
    "first_dim_core",
    "first_dim_finalize",
    "other_dim",
    "mod_switch",
)
PLOT_GROUPS = (
    ("query frontend", ("expand", "convert", "first_dim_query_ntt", "first_dim_query_pack")),
    ("scan", ("first_dim_core",)),
    ("finalize", ("first_dim_finalize",)),
    ("fold", ("other_dim", "mod_switch")),
    ("response", ("response_serialize_ms",)),
)


class RenderError(Exception):
    pass


def repo_root_from_this_file(path: Path) -> Path:
    for parent in path.resolve().parents:
        if (parent / "scripts" / "select_cuckoo_layout.py").exists():
            return parent
    raise RenderError("could not find repo root containing scripts/select_cuckoo_layout.py")


def load_selector(repo_root: Path) -> Any:
    selector_path = repo_root / "scripts" / "select_cuckoo_layout.py"
    spec = importlib.util.spec_from_file_location("select_cuckoo_layout", selector_path)
    if spec is None or spec.loader is None:
        raise RenderError(f"could not import {selector_path}")
    module = importlib.util.module_from_spec(spec)
    spec.loader.exec_module(module)
    return module


def load_json(path: Path) -> dict[str, Any]:
    try:
        data = json.loads(path.read_text())
    except FileNotFoundError as exc:
        raise RenderError(f"{path}: missing file") from exc
    except json.JSONDecodeError as exc:
        raise RenderError(f"{path}: invalid JSON: {exc}") from exc
    if not isinstance(data, dict):
        raise RenderError(f"{path}: JSON is not an object")
    return data


def p95(values: list[float]) -> float:
    ordered = sorted(values)
    return ordered[max(0, math.ceil(0.95 * len(ordered)) - 1)]


def mean(values: list[float]) -> float:
    return statistics.fmean(values)


def metric(data: dict[str, Any], path: Path, dotted_key: str) -> float:
    value: Any = data
    for part in dotted_key.split("."):
        if not isinstance(value, dict) or part not in value:
            raise RenderError(f"{path}: missing {dotted_key}")
        value = value[part]
    if not isinstance(value, (int, float)) or isinstance(value, bool):
        raise RenderError(f"{path}: {dotted_key} is not numeric")
    return float(value)


def bytes_metric(data: dict[str, Any], path: Path, dotted_key: str) -> int:
    value = metric(data, path, dotted_key)
    if not value.is_integer():
        raise RenderError(f"{path}: {dotted_key} is not an integer byte count")
    return int(value)


def summarize_pilot(path: Path, validated: dict[str, Any]) -> dict[str, Any]:
    data = load_json(path)
    samples = data["samples"]
    server_ms = [float(sample["server_total_ms"]) for sample in samples]
    response_ms = [float(sample["response_serialize_ms"]) for sample in samples]
    stage_means = {
        stage: mean([float(sample["stages_ms"][stage]) for sample in samples])
        for stage in STAGE_ORDER
    }
    group_means = {}
    for group, stages in PLOT_GROUPS:
        values = []
        for sample in samples:
            total = 0.0
            for stage in stages:
                if stage == "response_serialize_ms":
                    total += float(sample["response_serialize_ms"])
                else:
                    total += float(sample["stages_ms"][stage])
            values.append(total)
        group_means[group] = mean(values)

    return {
        "path": path,
        "height": validated["height"],
        "n0": int(data["bucket_layouts"][0]["layout"]["N0"]),
        "mean_server_ms": mean(server_ms),
        "median_server_ms": statistics.median(server_ms),
        "p95_server_ms": p95(server_ms),
        "sample_sd_server_ms": statistics.stdev(server_ms),
        "stage_means_ms": stage_means,
        "group_means_ms": group_means,
        "response_serialize_mean_ms": mean(response_ms),
        "modeled_query_bytes": bytes_metric(data, path, "communication.modeled_query_bytes"),
        "actual_response_bytes": bytes_metric(data, path, "communication.actual_response_bytes"),
        "modeled_helper_key_bytes": bytes_metric(data, path, "communication.modeled_helper_key_bytes"),
        "physical_db_bytes": bytes_metric(data, path, "storage.estimated_preprocessed_bytes"),
    }


def validate_selection(selection_path: Path, summaries: list[dict[str, Any]]) -> None:
    selection = load_json(selection_path)
    decision = selection.get("decision")
    if not isinstance(decision, dict):
        raise RenderError(f"{selection_path}: missing decision object")
    if decision.get("score") != "min_mean_complete_path_server_ms":
        raise RenderError(f"{selection_path}: decision.score is not min_mean_complete_path_server_ms")
    chosen_height = decision.get("chosen_expansion_height")
    ordered = sorted(
        summaries,
        key=lambda item: (item["mean_server_ms"], item["physical_db_bytes"], item["height"]),
    )
    expected_height = ordered[0]["height"]
    if chosen_height != expected_height:
        raise RenderError(
            f"{selection_path}: chosen height {chosen_height} != mean-based winner {expected_height}"
        )


def human_bytes(value: int) -> str:
    units = ("B", "KiB", "MiB", "GiB")
    amount = float(value)
    for unit in units:
        if amount < 1024.0 or unit == units[-1]:
            return f"{amount:.2f} {unit}" if unit != "B" else f"{value} B"
        amount /= 1024.0
    return f"{value} B"


def markdown_table(headers: list[str], rows: list[list[str]]) -> str:
    lines = [
        "| " + " | ".join(headers) + " |",
        "| " + " | ".join("---" for _ in headers) + " |",
    ]
    lines.extend("| " + " | ".join(row) + " |" for row in rows)
    return "\n".join(lines)


def render_markdown(root: Path, summaries: list[dict[str, Any]]) -> None:
    by_height = {item["height"]: item for item in summaries}
    baseline = by_height[7]["mean_server_ms"]
    ordered = sorted(summaries, key=lambda item: item["height"])
    winner = min(
        summaries,
        key=lambda item: (item["mean_server_ms"], item["physical_db_bytes"], item["height"]),
    )

    rows = []
    for item in ordered:
        rows.append([
            f"h_q={item['height']}",
            str(item["n0"]),
            f"{item['mean_server_ms'] / 1000.0:.3f}",
            f"{item['median_server_ms'] / 1000.0:.3f}",
            f"{item['p95_server_ms'] / 1000.0:.3f}",
            f"{item['sample_sd_server_ms'] / 1000.0:.3f}",
            f"{baseline / item['mean_server_ms']:.3f}x",
            human_bytes(item["modeled_query_bytes"]),
            human_bytes(item["actual_response_bytes"]),
            human_bytes(item["modeled_helper_key_bytes"]),
            human_bytes(item["physical_db_bytes"]),
        ])

    stage_rows = []
    for item in ordered:
        stage_rows.append([
            f"h_q={item['height']}",
            str(item["n0"]),
            *[f"{item['stage_means_ms'][stage] / 1000.0:.3f}" for stage in STAGE_ORDER],
        ])

    lines = [
        "# Cuckoo 1 GiB Layout Pilot Comparison",
        "",
        "Four forced-expansion Cuckoo pilot layouts, 16 measured pilot samples each with 3 warmups. These are pilot measurements, not 100-sample formal results.",
        "",
        f"Winner by mean complete-path server time: h{winner['height']} (N0={winner['n0']}).",
        "",
        "Run context: the 20m8.7s pause was the generic preflight, not these N0 pilots; the N0 pilots ran serially against each other. The generic run has resumed, and the formal 100-sample tuned Cuckoo result has not completed yet.",
        "",
        "## Complete-Path Server Metrics",
        "",
        markdown_table(
            [
                "height",
                "N0",
                "mean server s",
                "median server s",
                "p95 server s",
                "sample SD s",
                "speedup vs N0=64",
                "query modeled",
                "response actual",
                "setup keys modeled",
                "physical DB bytes",
            ],
            rows,
        ),
        "",
        "## Eight-Stage Server Means",
        "",
        markdown_table(
            ["height", "N0", *[stage.replace("_", " ") + " s" for stage in STAGE_ORDER]],
            stage_rows,
        ),
        "",
        "## Figures",
        "",
        "- `stage-comparison.png`",
        "- `stage-comparison.pdf`",
        "",
        "Selection was verified against `selection.json` using mean complete-path server time. No paired confidence interval is reported because the pilots are not an interleaved paired-block design.",
    ]
    (root / "TUNING_COMPARISON.md").write_text("\n".join(lines) + "\n")


def render_plot(root: Path, summaries: list[dict[str, Any]]) -> None:
    import matplotlib.pyplot as plt
    import numpy as np

    ordered = sorted(summaries, key=lambda item: item["height"])
    labels = [f"h_q={item['height']}\nN0={item['n0']}" for item in ordered]
    x = np.arange(len(ordered))
    bottoms = np.zeros(len(ordered))
    colors = {
        "query frontend": "#4C78A8",
        "scan": "#F58518",
        "finalize": "#54A24B",
        "fold": "#B279A2",
        "response": "#E45756",
    }

    fig, ax = plt.subplots(figsize=(9.2, 5.8), constrained_layout=True)
    for group, _ in PLOT_GROUPS:
        values = np.array([item["group_means_ms"][group] / 1000.0 for item in ordered])
        ax.bar(x, values, bottom=bottoms, label=group, color=colors[group])
        bottoms += values

    means = np.array([item["mean_server_ms"] / 1000.0 for item in ordered])
    ax.plot(x, means, color="#222222", marker="o", linewidth=1.4, label="server total mean")
    for idx, value in enumerate(means):
        ax.text(idx, bottoms[idx] + max(means) * 0.012, f"{value:.2f}s", ha="center", va="bottom", fontsize=9)

    ax.set_title("Cuckoo 1 GiB forced-layout pilot: 16 samples per height")
    ax.set_ylabel("seconds per complete Merkle path")
    ax.set_xticks(x, labels)
    ax.set_ylim(0, max(bottoms) * 1.12)
    ax.grid(axis="y", color="#dddddd", linewidth=0.8)
    ax.set_axisbelow(True)
    ax.legend(loc="upper center", bbox_to_anchor=(0.5, -0.08), ncol=3, frameon=False)
    fig.savefig(root / "stage-comparison.png", dpi=180)
    fig.savefig(root / "stage-comparison.pdf")
    plt.close(fig)


def parse_args() -> argparse.Namespace:
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument(
        "--root",
        type=Path,
        default=Path(__file__).resolve().parent,
        help="directory containing h7-h10 pilot JSON/process files and selection.json",
    )
    return parser.parse_args()


def main() -> int:
    args = parse_args()
    root = args.root.resolve()
    selector_error_type: type[Exception] = Exception
    try:
        os.environ.setdefault("MPLCONFIGDIR", "/private/tmp/cuckoo-mpl")
        repo_root = repo_root_from_this_file(Path(__file__))
        selector = load_selector(repo_root)
        selector_error_type = selector.SelectionError
        pilot_paths = [root / f"h{height}.json" for height in EXPECTED_HEIGHTS]
        validated = [
            selector.validate_pilot(path, EXPECTED_TRIALS, EXPECTED_WARMUPS, EXPECTED_LEAF_COUNT)
            for path in pilot_paths
        ]
        selector.validate_same_inputs(validated)
        summaries = [
            summarize_pilot(path, item)
            for path, item in zip(pilot_paths, validated)
        ]
        validate_selection(root / "selection.json", summaries)
        render_markdown(root, summaries)
        render_plot(root, summaries)
        print(f"wrote {root / 'TUNING_COMPARISON.md'}")
        print(f"wrote {root / 'stage-comparison.png'}")
        print(f"wrote {root / 'stage-comparison.pdf'}")
        return 0
    except (RenderError, selector_error_type) as exc:
        print(f"error: {exc}", file=sys.stderr)
        return 1


if __name__ == "__main__":
    raise SystemExit(main())
