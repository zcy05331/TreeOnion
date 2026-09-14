#!/usr/bin/env python3
"""Validate the completed native packing comparison and write its compact tables."""
import json
from pathlib import Path

ROOT = Path(__file__).resolve().parent

def main():
    rows = []
    stages = []
    summaries = {}
    for name, expected_ks in (("h3", 27), ("h1", 79)):
        data = json.loads((ROOT / "native" / "outputs" / (name + "-packing") / "comparison.json").read_text())
        assert data["state"] == "complete", (name, data["state"])
        a, b = data["summary"]["projection"], data["summary"]["mlwe"]
        assert a["completed_samples"] == b["completed_samples"] == 100
        assert a["wire"] == b["wire"]
        assert a["key_metadata"] == b["key_metadata"]
        for mode in ("projection", "mlwe"):
            for round_id in (0, 1):
                raw = json.loads((ROOT / "native" / "outputs" / (name + "-packing") / f"round{round_id}-{mode}.json").read_text())
                candidate = next(x for x in raw["candidates"] if x.get("eligible") and x.get("samples"))
                assert len(candidate["samples"]) == 50
                assert {x["response_payload_bytes"] for x in candidate["samples"]} == {11264}
                expected_count = expected_ks if mode == "mlwe" else 0
                assert {x["response_merge_keyswitches"] for x in candidate["samples"]} == {expected_count}
        assert b["counter_metadata"]["response_merge_keyswitches_mean"] == expected_ks
        assert b["stages_ms_mean"]["project"] == b["stages_ms_mean"]["pack"] == 0
        assert a["stages_ms_mean"]["merge"] == 0
        logical_mib = (2 * data["layout"]["leaf_count"] - 1) * 32 / 2**20
        result = {"layout": data["layout"], "projection": a, "mlwe": b}
        for mode, v in (("projection", a), ("mlwe", b)):
            v["throughput_mib_s"] = logical_mib * 1000 / v["server_total_ms_mean"]
            v["process_peak_rss_bytes_max"] = max(x["process_peak_rss_bytes"] for x in v["process_records"])
            stages.append((name, mode, v["stages_ms_mean"]))
        result["server_time_reduction_percent"] = 100 * (1 - b["server_total_ms_mean"] / a["server_total_ms_mean"])
        result["throughput_gain_percent"] = 100 * (a["server_total_ms_mean"] / b["server_total_ms_mean"] - 1)
        summaries[name] = result
        rows.append(f"| {name} | {a['server_total_ms_mean']:.3f} | {b['server_total_ms_mean']:.3f} | {result['throughput_gain_percent']:+.2f}% | {a['throughput_mib_s']:.2f} | {b['throughput_mib_s']:.2f} |")
    (ROOT / "SUMMARY.json").write_text(json.dumps(summaries, indent=2) + "\n")
    lines = ["# Wisconsin MLWE response merge results", "", "Source: 0ac2393c09c597b5bb1f41129577117a43f4770a. Native Xeon Silver 4314, CPU 0; 1 GiB logical tree, 32 B entries, EP2/drop36, exact KS8. Each method/layout has 100 measured queries (two reversed-order rounds of 50), six warmups. Every requested path decoded correctly.", "", "| Layout | Projection server ms | MLWE server ms | Throughput gain | Projection MiB/s | MLWE MiB/s |", "|---|---:|---:|---:|---:|---:|", *rows, "", "| Stage (ms) | h3 Projection | h3 MLWE | h1 Projection | h1 MLWE |", "|---|---:|---:|---:|---:|"]
    for stage in ("expand", "complete", "prepare", "scan", "finalize", "fold", "rotate", "project", "pack", "merge", "switch"):
        lines.append("| " + stage + " | " + " | ".join(f"{s[stage]:.3f}" for _, _, s in stages) + " |")
    lines += ["", "Both methods use the same database layout, query schedule, q/t, gadget parameters and registered key family. h3 is G64/P1/N0=352/b9; h1 is G8/P3/N0=512/b7. The current planner was not retuned for MLWE.", "", "Projection/merge KS counts: h3 105 -> 27; h1 492 -> 79. Private rotation remains unchanged. The new merge includes normalization, public shifts, additions and shared BV key switches. Its input fragments use public bit-reversed residue positions at decode.", "", "Communication is unchanged: each reply contains 11,264 ciphertext-payload bytes; seeded query/key costs retain their existing modeled definitions. Framing/packing metadata is not included in the ciphertext payload. Peak process RSS (including setup) is recorded in SUMMARY.json.", "", "Local 256 MiB Rosetta results are preserved in LOCAL_RESULTS.md: the changed response stage became faster, but full-query time did not improve during concurrent local history-cleanup work. Native results above are the dedicated-node comparison."]
    h1 = summaries["h1"]
    old, new = h1["projection"], h1["mlwe"]
    direct = old["stages_ms_mean"]["project"] + old["stages_ms_mean"]["pack"] - new["stages_ms_mean"]["merge"]
    scan_delta = old["stages_ms_mean"]["scan"] - new["stages_ms_mean"]["scan"]
    lines += ["", f"For h1, the changed Projection/packing -> merge stages directly save {direct:.3f} ms. The measured full-query saving is {old['server_total_ms_mean'] - new['server_total_ms_mean']:.3f} ms, and the scan also measures {scan_delta:.3f} ms faster. The scan kernel itself is unchanged; this additional difference has not been attributed to cache, allocation or frequency effects. Thus 7.93% is the observed whole-query throughput gain for this fixed configuration, not a claim that fewer KS alone account for all of it."]
    (ROOT / "RESULTS.md").write_text("\n".join(lines) + "\n")
    print("\n".join(rows))

if __name__ == "__main__":
    main()
