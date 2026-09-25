#!/usr/bin/env python3
"""Emit RESULTS-ICC.md from a local sync of the Campus Cluster runs directory.

usage: write_results_icc.py RUNS_DIR OUT.md
RUNS_DIR holds one directory per pool job as synced from <root>/runs. Final
rows: primary-v2 (Ours), core-slf-ep3, core-merged-ep3, core-packed-ep3 and
core-cuckoo-ep3 (the OnionPIRv2 baselines on the ep3-s32 selector gadget),
treepir, spiral-pbc, kspir-pbc, via-b, via-pbc, respire-small and respire-4g,
with the 8 GiB rows in the <job>-8g runs. The earlier primary and exact-EP6
core-slf, core-merged and core-cuckoo runs feed the gadget comparison and the
same-gadget comparison with Utah.
"""
import glob
import json
import statistics as st
import sys
from pathlib import Path

RUNS, OUT = Path(sys.argv[1]), Path(sys.argv[2])
SIZES = [("256mb", "256mib", 22, "256 MiB"), ("512mb", "512mib", 23, "512 MiB"), ("1gb", "1gib", 24, "1 GiB"),
         ("2gb", "2gib", 25, "2 GiB"), ("4gb", "4gib", 26, "4 GiB"), ("8gb", "8gib", 27, "8 GiB")]
NS = len(SIZES)
UTAH = {  # RESULTS-ADDENDUM.md (Xeon Gold 5512U), MB/s; Ours on ep3-s32, the OnionPIRv2 baselines on exact EP6
    "Ours h1 g8/P3": [769, 952, 1120, 1220, 1273],
    "Standard OnionPIR (single block)": [985, 1163, 1317, 1416, 1458],
    "Layerwise tuned": [503, 666, 859, 1033, 1196],
    "Layerwise + response merging": [389, 522, 656, 762, 835],
    "Cuckoo batch tuned": [136, 187, 240, 297, 358],
    "TreePIR": [132, 162, 196, 223, 177],
    "Flat": [45, 52, 54, 57, 56],
    "Respire": [19, 30, 42, 62, None],
}
# Estimated from the 4 GiB peaks; these rows were not attempted at 8 GiB.
OVER_MEMORY = {"Spiral + PBC": "about 336 GiB", "Respire": "about 400 GiB"}


def load(p):
    p = Path(p)
    return json.loads(p.read_text()) if p.exists() else None


def logical(h):
    return (2 ** (h + 1) - 1) * 32


T = {}  # final rows
G = {}  # earlier runs: Ours' first run and the exact-EP6 baselines


def put(table, scheme, i, ms, h, q, r, k, note=""):
    table.setdefault(scheme, [None] * NS)[i] = dict(ms=ms, tp=logical(h) / ms / 1e3, q=q, r=r, k=k, note=note)


def job(base, c, final="{}"):
    """Run directory of a scheme at size c: 8 GiB rows live in <base>-8g."""
    return f"{base}-8g" if c == "8gb" else final.format(base)


# The TreePIR row uses VBPIR only when the VBPIR run completed every size;
# otherwise it stays on the SealPIR d=2 runs.
TREEPIR_VBPIR = all((RUNS / f"treepir-vbpir/outputs/treepir-backend-tuning/{c}/formal.json").exists() for c, *_ in SIZES)


def primary(table, i, run, p, h):
    best = None
    for f in glob.glob(str(RUNS / f"{run}/outputs/primary/{p}/formal/*.json")):
        if f.endswith("-process.json"):
            continue
        d = load(f)
        ms = st.mean(x["server_total_ms"] for x in d["samples"])
        if best is None or ms < best[0]:
            best = (ms, d)
    if best:
        ms, d = best
        cm = d["communication"]
        put(table, "Ours h1 g8/P3", i, ms, h, cm["modeled_query_bytes_per_trial"], cm["actual_response_bytes_per_trial"],
            cm["modeled_helper_key_bytes"], f"N0 {d['metadata']['N0']}, {d['metadata']['query_layout']}")


def core(table, i, c, h, run_of):
    for name, fn, run in (("Standard OnionPIR (single block)", "standard", "core-slf"),
                          ("Layerwise tuned", "layerwise_tuned", "core-slf"),
                          ("Layerwise + response merging", "layerwise_merged_tuned", "core-merged"),
                          ("Layerwise + merging + packed query", "layerwise_packed_tuned", "core-packed"),
                          ("Flat", "flat", "core-slf")):
        d = load(RUNS / f"{run_of(run)}/outputs/core-matrix/{c}/{fn}.json")
        if d:
            cs = d["cases"][0]
            put(table, name, i, cs["server_compute_ms"], h, cs["online_query_bytes_modeled"], cs["online_response_bytes_actual"],
                cs["helper_key_bytes_modeled"],
                f"{cs['pir_call_count']} call{'s' if cs['pir_call_count'] != 1 else ''}, h{d['workload'].get('reference_expansion_height')}")
    d = load(RUNS / f"{run_of('core-cuckoo')}/outputs/core-matrix/{c}/cuckoo_tuned.json")
    if d:
        cm = d["communication"]
        put(table, "Cuckoo batch tuned", i, st.mean(x["server_total_ms"] for x in d["samples"]), h, cm["modeled_query_bytes"],
            cm["actual_response_bytes"], cm["modeled_helper_key_bytes"], f"{d['num_buckets']} buckets, h{d['requested_expansion_height']}")


VIA_ERR = {}  # (scheme, size index) -> largest decoding error in q4 units
for i, (c, p, h, _) in enumerate(SIZES):
    primary(T, i, job("primary", c, "primary-v2"), p, h)
    core(T, i, c, h, lambda run: job(run, c, "{}-ep3"))
    if c != "8gb":
        primary(G, i, "primary", p, h)
        core(G, i, c, h, lambda run: run)
    d = load(RUNS / f"{'treepir-vbpir' if TREEPIR_VBPIR else job('treepir', c)}/outputs/treepir-backend-tuning/{c}/formal.json")
    if d:
        res = [r for r in d["results"] if r["phase"] == "measured"]
        put(T, "TreePIR", i, d["measured_summary"]["server_ms_mean"], h,
            st.mean(r["actual_query_payload_bytes_unseeded"] for r in res),
            d["measured_summary"]["actual_response_payload_bytes_mean"], d["actual_helper_key_bytes_uncompressed"],
            d["backend_label"])
    for name, run, fn in (("Spiral + PBC", "spiral-pbc", "spiral_pbc"), ("KsPIR + PBC", "kspir-pbc", "kspir_pbc"),
                          ("VIA-B", "via-b", "via_b"), ("VIA + PBC", "via-pbc", "via_pbc")):
        d = load(RUNS / f"{job(run, c)}/outputs/{run}/{c}/{fn}.json")
        if d:
            cm = d["communication"]
            if fn == "kspir_pbc":
                kp = d["kspir_parameters"]
                note = f"r {kp['packing_r']}, N1 {kp['bsgs_n1']}, {d['compiler_label']}"
            elif fn.startswith("via"):
                vp = d["via_parameters"]
                note = f"I {vp['rows_I']}, J {vp['columns_J']}, {d['batch']['queries_per_path']} queries, {d['compiler_label']}"
                VIA_ERR[(name, i)] = d["measured_summary"]["max_error_q4"]
            else:
                sj = d["spiral_parameters"]["json"]
                note = f"nu_1 {sj['nu_1']}, nu_2 {sj['nu_2']}, instances {sj['instances']}"
            put(T, name, i, d["measured_summary"]["server_ms_mean"], h, cm["query_bytes_per_path"], cm["response_bytes_per_path"],
                cm["public_parameter_bytes"], note)
for run in ("respire-small", "respire-4g"):
    s = load(RUNS / f"{run}/outputs/external-matrix/summary.json")
    for r in (s or {}).get("summary_rows", []):
        sc, mode = r["scale_mode"].split(":")
        i = [x[1] for x in SIZES].index(sc)
        if mode == "respire":
            put(T, "Respire", i, r["server_ms"], SIZES[i][2], r["query_bytes"], r["response_bytes"],
                (11 * 24 + 2 * 4 + 8 * 2) * 14336, "")

OURS = "Ours h1 g8/P3"
ORDER = [OURS, "Standard OnionPIR (single block)", "Layerwise tuned", "Cuckoo batch tuned", "Spiral + PBC",
         "KsPIR + PBC", "VIA-B", "VIA + PBC", "TreePIR", "Flat", "Respire"]
ABLATION = ["Layerwise tuned", "Layerwise + response merging", "Layerwise + merging + packed query", OURS]
GADGET = ["Standard OnionPIR (single block)", "Layerwise tuned", "Layerwise + response merging", "Cuckoo batch tuned", "Flat"]
COMM = ORDER + ABLATION[1:3]
HDR = "| " + " | ".join(s[3] for s in SIZES) + " |"


def cell(scheme, i, v, key, fmt):
    if v is None or v.get(key) is None:
        return "over memory" if SIZES[i][0] == "8gb" and scheme in OVER_MEMORY else "—"
    return fmt(v[key])


def kb(x):
    if x == 0:
        return "none"
    return f"{x / 1024:.1f} KB" if x < 1024 * 1024 else f"{x / 1024 / 1024:.2f} MB"


def table(title, key, fmt, order=ORDER, level="##"):
    out = [f"\n{level} {title}\n", "| Scheme " + HDR, "|---|" + "---:|" * NS]
    for k in order:
        v = T.get(k)
        if v:
            out.append(f"| {k} | " + " | ".join(cell(k, i, x, key, fmt) for i, x in enumerate(v)) + " |")
    return out


def ratios(a, b, table_a=T, table_b=T, n=NS):
    return [x["tp"] / y["tp"] for x, y in zip(table_a[a][:n], table_b[b][:n]) if x and y]


def span(values, fmt, sep="–"):
    lo, hi = min(values), max(values)
    return fmt(lo) if fmt(lo) == fmt(hi) else f"{fmt(lo)}{sep}{fmt(hi)}"


def pct(x):
    return f"{100 * x:.0f}%"


def over_row(k):
    return [x for x in T[k] if x]


def online(k):
    return [x["q"] + x["r"] for x in over_row(k)]


# ---------------------------------------------------------------- findings
lw = [x["tp"] / y["tp"] for x, y in zip(T[OURS], T["Layerwise tuned"])]
std = ratios(OURS, "Standard OnionPIR (single block)")
lift = {k: [x["tp"] / y["tp"] - 1 for x, y in zip(T[k][:-1], G[k][:-1])] for k in GADGET}
ours_moved = [x["tp"] / y["tp"] - 1 for x, y in zip(T[OURS][:-1], G[OURS][:-1])]
via = ratios("VIA + PBC", OURS)
CAL = {}
for run in RUNS.iterdir():
    d = load(run / "outputs/calibration/1gb/standard.json")
    if d:
        CAL[run.name] = d["cases"][0]["server_compute_ms"]
cal_pair = CAL["core-slf-8g"] / CAL["primary-8g"] - 1
F = [
    f"- **Ours against Layerwise tuned**, both on ep3-s32: {', '.join(f'{r:.2f}x' for r in lw)} from 256 MiB to 8 GiB. "
    f"Layerwise is faster at 8 GiB; it makes {T['Layerwise tuned'][-1]['note'].split(',')[0]} and sends "
    f"{kb(online('Layerwise tuned')[-1])} online per path against our {kb(online(OURS)[-1])}. The two 8 GiB jobs "
    f"ran on different nodes whose calibration rows differ by {100 * abs(cal_pair):.1f}%.",
    f"- **Node consistency**: the Standard 1 GiB calibration row spans {min(CAL.values()):.1f}–{max(CAL.values()):.1f} ms "
    f"({100 * (max(CAL.values()) / min(CAL.values()) - 1):.1f}%) over {len(CAL)} jobs on "
    f"{len({(RUNS / r / 'metadata/node.txt').read_text().strip() for r in CAL})} nodes.",
    f"- **Ours against Standard OnionPIR** (one block, a speed reference, not a path protocol): "
    f"{', '.join(f'{r:.2f}x' for r in std)} from 256 MiB to 8 GiB.",
    "- **Selector gadget**: moving the OnionPIRv2 baselines from exact EP6 to ep3-s32 raises them by " +
    "; ".join(f"{k.split(' (')[0]} {span(v, lambda x: f'{100 * x:+.0f}%', ' to ')}" for k, v in lift.items()) +
    f" (256 MiB to 4 GiB). Ours ran ep3-s32 in both runs and moved by {span(ours_moved, lambda x: f'{100 * x:+.1f}%', ' to ')}.",
    f"- **Fastest baseline outside the OnionPIRv2 family**: VIA + PBC at {span(via, pct)} of our throughput "
    f"({span([x['tp'] for x in over_row('VIA + PBC')], lambda t: f'{t:.0f}')} MB/s), with "
    f"{span(online('VIA + PBC'), kb)} online per path and no offline keys.",
    f"- **Lowest online communication**: VIA-B, {span(online('VIA-B'), kb)} per path after "
    f"{kb(T['VIA-B'][0]['k'])} of offline keys, at {span([x['tp'] for x in over_row('VIA-B')], lambda t: f'{t:.0f}')} MB/s. "
    f"Ours sends {span(online(OURS), kb)} at every size after {kb(T[OURS][0]['k'])} of keys.",
    f"- **Ablation**: the merged Layerwise step reaches {span(ratios('Layerwise + response merging', OURS), pct)} of our "
    f"throughput, and packing its queries cuts its online traffic from "
    f"{span(online('Layerwise + response merging'), kb)} to {span(online('Layerwise + merging + packed query'), kb)} "
    f"at the same speed ({span(ratios('Layerwise + merging + packed query', 'Layerwise + response merging'), lambda x: f'{x:.2f}x')}).",
]

L = ["# Campus Cluster results (Sept 22-24 2026)\n",
     "Nodes ccc0400-ccc0415 (Xeon Platinum 8358, 2 x 32 cores, 251 GiB), whole-node exclusive jobs, one thread pinned to "
     "CPU 0 with memory on node 0; every job checks the node configuration and repeats a Standard 1 GiB calibration row. "
     "Seed-7 leaf schedule; logical database = full tree, (2^(H+1)-1) x 32 B; throughput = logical bytes / mean server "
     "time per complete path. Ours and the OnionPIRv2 baselines run the ep3-s32 selector gadget, built on each job's node.\n",
     "## Findings\n", *F]
L += table("Throughput (MB/s)", "tp", lambda t: f"{t:.0f}")
L += table("Server time per path (ms)", "ms", lambda t: f"{t:.1f}")
L += table("Ablation: the primary algorithm's steps on the Layerwise baseline (MB/s)", "tp", lambda t: f"{t:.0f}", ABLATION)
L.append("\nLayerwise + response merging adds the primary algorithm's rotation and MLWE merge on the transposed layout "
         "with one shared gamma selector; the packed variant also packs every level's query into shared expansion "
         "slots. Both reuse the primary algorithm's selectors, so they are ablation steps, not baselines.")
L.append("\n## Selector gadget (MB/s, exact EP6 / ep3-s32)\n")
L += ["| Scheme " + "| " + " | ".join(s[3] for s in SIZES[:-1]) + " |", "|---|" + "---:|" * (NS - 1)]
for k in [OURS] + GADGET:
    old, new = G.get(k), T.get(k)
    if old and new:
        L.append(f"| {k} | " + " | ".join("—" if not x or not y else f"{x['tp']:.0f} / {y['tp']:.0f}"
                                           for x, y in zip(old[:-1], new[:-1])) + " |")
L.append("\nThe Ours row compares its first run with the rerun from the same build as the baselines; both use ep3-s32.")
L.append("\n## Communication\n")
L += table("Offline, once per session", "k", kb, COMM, "###")
L += table("Online query, per path", "q", kb, COMM, "###")
L += table("Online response, per path", "r", kb, COMM, "###")
L.append("\n### Online total, per path\n")
L += ["| Scheme " + HDR, "|---|" + "---:|" * NS]
for k in COMM:
    v = T.get(k)
    if v:
        L.append(f"| {k} | " + " | ".join(cell(k, i, None if not x else {"t": x["q"] + x["r"]}, "t", kb)
                                           for i, x in enumerate(v)) + " |")
L.append("\n## Against the Utah run, same gadget (ICC MB/s / Utah MB/s)\n")
L.append("Ours from its first run and the OnionPIRv2 baselines from the exact-EP6 runs, as at Utah, so the ratio is the "
         "node alone. Utah's TreePIR used VBPIR below 4 GiB; ICC's uses SealPIR d=2 throughout.\n")
L += ["| Scheme " + "| " + " | ".join(s[3] for s in SIZES[:-1]) + " |", "|---|" + "---:|" * (NS - 1)]
for k in [OURS] + GADGET + ["TreePIR", "Respire"]:
    v, u = (G if k in G else T).get(k), UTAH.get(k)
    if v and u:
        L.append(f"| {k} | " + " | ".join("—" if not x or not y else f"{x['tp'] / y:.2f}" for x, y in zip(v[:-1], u)) + " |")
L.append("\n## Calibration and provenance (Standard 1 GiB, one row per job)\n")
L += ["| Job | node | source | server ms |", "|---|---|---|---:|"]
for run in sorted(p.name for p in RUNS.iterdir() if p.is_dir()):
    d = load(RUNS / f"{run}/outputs/calibration/1gb/standard.json")
    node, src = RUNS / f"{run}/metadata/node.txt", RUNS / f"{run}/metadata/source-commit.txt"
    if d:
        L.append(f"| {run} | {node.read_text().strip() if node.exists() else '?'} | "
                 f"{src.read_text().strip()[:7] if src.exists() else '?'} | {d['cases'][0]['server_compute_ms']:.1f} |")
L.append("\n## Row notes\n")
for k in COMM:
    v = T.get(k)
    if v:
        notes = [f"{SIZES[i][3]}: {x['note']}" for i, x in enumerate(v) if x and x.get("note")]
        if notes:
            L.append(f"- {k}: " + "; ".join(notes))
via_err = [e for (k, _), e in VIA_ERR.items() if k == "VIA + PBC"]
L += ["\n## Limitations\n",
      "- Not measured: Spiral + PBC and Respire at 8 GiB (" + ", ".join(f"{k} {v}" for k, v in OVER_MEMORY.items()) +
      " estimated from the 4 GiB peaks, against 251 GiB per node). TreePIR + VBPIR fails with a zero noise budget at "
      "4 and 8 GiB (72 and 138 GiB peak, not memory), so the TreePIR row uses SealPIR d=2 at every size.",
      f"- VIA + PBC uses a ternary S2 instead of the paper's sigma-4960 key, which exceeds the CMux bound "
      f"(scripts/path_baselines/via_pbc/README.md); its largest decoding error was {span(via_err, str)} of 64 q4 units.",
      "- The first core-slf, core-merged, core-cuckoo and primary runs used exact EP6 for the baselines and a 32-bit-seeded "
      "RNG; they appear only in the gadget and Utah comparisons."]
OUT.write_text("\n".join(L) + "\n")
print("\n".join(L[:12]))
