import json
import os
import statistics
from pathlib import Path

os.environ.setdefault("MPLCONFIGDIR", "/private/tmp/layerwise-scaling-mpl")
import matplotlib
matplotlib.use("Agg")
import matplotlib.pyplot as plt

HERE = Path(__file__).resolve().parent
ROOT = HERE.parent / "profiling"
GROUPS = {
    "Query preparation": ["expand", "convert", "first_dim_query_ntt", "first_dim_query_pack"],
    "Scan": ["first_dim_core"],
    "Finalization": ["first_dim_finalize"],
    "Fold": ["other_dim"],
    "Response and glue": ["mod_switch"],
}
COLORS = ["#4c78a8", "#f58518", "#54a24b", "#b279a2", "#888888"]


def read_case(scale, case):
    folder = ROOT / scale
    data = json.loads((folder / f"{case}.json").read_text())
    process = json.loads((folder / f"{case}-process.json").read_text())
    row = data["cases"][0]
    samples = row["server_compute_samples_ms"]
    assert process["exit_code"] == 0 and row["correctness_passed"]
    assert len(samples) == 100 and data["workload"]["warmups"] == 3
    mean = statistics.fmean(samples)
    stages = row["pipeline_profile_ms"]
    parts = {name: sum(stages[k] for k in keys) for name, keys in GROUPS.items()}
    residual = mean - sum(parts.values())
    assert residual >= -0.001, (scale, case, residual)
    parts["Response and glue"] += residual
    return row, mean, parts


fig, axes = plt.subplots(1, 2, figsize=(10, 4.8))
lines = ["# Layerwise tuning across database scales", "",
         "Each configuration completed 100 correct complete-path queries after 3 warmups. Selection minimized measured mean server time.", "",
         "| Scale | Previous mean s | Tuned mean s | Time reduction | Speedup |",
         "| --- | ---: | ---: | ---: | ---: |"]
traffic = []
stage_rows = []
for ax, scale, label in zip(axes, ("1gb", "8gb"), ("1 GiB", "8 GiB")):
    before, old_mean, old_parts = read_case(scale, "layerwise")
    after, new_mean, new_parts = read_case(scale, "layerwise_tuned")
    assert before["sample_leaf_indices"] == after["sample_leaf_indices"]
    for key in ("online_query_bytes_modeled", "online_response_bytes_actual", "helper_key_bytes_modeled"):
        assert before[key] == after[key]
    lines.append(f"| {label} | {old_mean/1000:.3f} | {new_mean/1000:.3f} | {100*(1-new_mean/old_mean):.3f}% | {old_mean/new_mean:.3f}x |")
    traffic.append(f"| {label} | {after['online_query_bytes_modeled']} | {after['online_response_bytes_actual']} | {after['helper_key_bytes_modeled']} |")
    bottom = [0.0, 0.0]
    for name, color in zip(GROUPS, COLORS):
        heights = [old_parts[name]/1000, new_parts[name]/1000]
        ax.bar([0, 1], heights, bottom=bottom, color=color, width=0.55, label=name)
        bottom = [x+y for x, y in zip(bottom, heights)]
        stage_rows.append(f"| {label} | {name} | {old_parts[name]:.3f} | {new_parts[name]:.3f} |")
    for x, value in enumerate((old_mean, new_mean)):
        ax.text(x, value/1000 + old_mean/1000*0.025, f"{value/1000:.3f}s", ha="center")
    ax.set_xticks([0, 1], ["Previous", "Tuned"])
    ax.set_title(label)
    ax.set_ylim(0, old_mean/1000*1.15)
    ax.set_ylabel("Seconds per complete path")
    ax.grid(axis="y", alpha=0.2)
    ax.set_axisbelow(True)
fig.suptitle("Layerwise: 100 measured queries per configuration")
handles, labels = axes[0].get_legend_handles_labels()
fig.legend(handles, labels, loc="lower center", ncol=3, frameon=False)
fig.tight_layout(rect=(0, 0.13, 1, 0.93))
fig.savefig(HERE / "layerwise-stages.png", dpi=180, bbox_inches="tight")
fig.savefig(HERE / "layerwise-stages.pdf", bbox_inches="tight")
plt.close(fig)

lines += ["", "## Stage means", "",
          "| Scale | Stage group | Previous ms | Tuned ms |", "| --- | --- | ---: | ---: |"] + stage_rows
lines += ["", "## Communication", "",
          "Online byte counts are unchanged by tuning at each scale. Key estimates are one-time setup costs.", "",
          "| Scale | Modeled query bytes | Actual response bytes | Modeled helper-key bytes |",
          "| --- | ---: | ---: | ---: |"] + traffic
lines += ["", "## Measurement scope", "",
          "All runs used the same CloudLab host, CPU 0 and interleaved NUMA memory. Previous and tuned measurements were separate processes; speedups are descriptive ratios of measured arithmetic means. The two panels use separate vertical scales. Response and glue includes modulus switching and the residual between the full server timer and named stages.", "",
          "![Stage comparison](layerwise-stages.png)", ""]
(HERE / "COMPARISON.md").write_text("\n".join(lines))
print("Layerwise comparison validated and rendered")
