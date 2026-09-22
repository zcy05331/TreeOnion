#!/usr/bin/env python3
"""Summarize the Utah addendum root (synced locally) next to the completed campaign root."""
import json, statistics as st, sys, glob, os
from pathlib import Path
new = Path(sys.argv[1]); old = Path(sys.argv[2])
def mean(xs): return st.mean(xs)
def load(p):
    try: return json.load(open(p))
    except FileNotFoundError: return None

print("== A/B scan scratch (ms, mean over 50 trials; scan_core / server_total)")
for f in sorted(glob.glob(str(new/"outputs/ab-scratch/*.json"))):
    if f.endswith("-process.json"): continue
    d=load(f); s=d["samples"]
    print(f"  {Path(f).stem:16s} scan_core {mean(x['stages_ms']['scan_core'] for x in s):8.1f} sd {st.pstdev([x['stages_ms']['scan_core'] for x in s]):5.1f}   total {mean(x['server_total_ms'] for x in s):8.1f}  correct {all(x['correct'] for x in s)}")

scales=[("256mb","256mib",22),("512mb","512mib",23),("1gb","1gib",24),("2gb","2gib",25),("4gb","4gib",26)]
def primary(root, p):
    rows={}
    for f in glob.glob(str(root/f"outputs/primary/{p}/formal/*.json")):
        if f.endswith("-process.json"): continue
        d=load(f); s=d["samples"]; lay=d["metadata"]["query_layout"]
        rows[lay]=(mean(x["server_total_ms"] for x in s), d["actual_logical_bytes"], d["metadata"]["N0"], all(x["correct"] for x in s))
    return rows
def standard(root, c, sub="core-matrix"):
    d=load(root/f"outputs/{sub}/{c}/standard.json")
    if not d: return None
    cs=d["cases"][0]; w=d["workload"]
    return (cs["server_compute_ms"], cs["raw_dataset_bytes"], w.get("reference_expansion_height"), w.get("reference_fst_dim_sz"), cs["correctness_passed"], cs["pipeline_profile_ms"]["first_dim_core"])
def merkle(root,c,name):
    d=load(root/f"outputs/core-matrix/{c}/{name}.json")
    if not d: return None
    cs=d["cases"][0]; return (cs["server_compute_ms"], cs["raw_dataset_bytes"], cs["correctness_passed"])
def cuckoo(root,c):
    d=load(root/f"outputs/core-matrix/{c}/cuckoo_tuned.json")
    if not d: return None
    s=d["samples"]; return (mean(x["server_total_ms"] for x in s), d["actual_logical_bytes"], d["requested_expansion_height"], all(x["correct"] for x in s))
def treepir(root,c):
    d=load(root/f"outputs/treepir-backend-tuning/{c}/formal.json")
    if not d: return None
    m=d["measured_summary"]; return (m["server_ms_mean"], d["backend_label"], m["all_correct"])
def external(root):
    d=load(root/"outputs/external-matrix/summary.json")
    return {r["scale_mode"]:(r["server_ms"], r["logical_mib_s"]*1.048576) for r in d["summary_rows"]} if d else {}
ext_old=external(old); ext_new=external(new)

print("\n== Primary (ours) formal rows: old campaign binary vs rebuilt binary (ms; MB/s)")
for c,p,h in scales:
    o=primary(old,p); n=primary(new,p); B=(2**(h+1)-1)*32
    for lay in ("split-columns","interleaved"):
        oo=o.get(lay); nn=n.get(lay)
        so=f"{oo[0]:8.1f} ({B/oo[0]/1e3:6.0f})" if oo else "       -           "
        sn=f"{nn[0]:8.1f} ({B/nn[0]/1e3:6.0f})  N0 {nn[2]} ok={nn[3]}" if nn else "-"
        print(f"  {p:7s} {lay:13s} old {so}   new {sn}")

print("\n== Standard: default rule vs pinned h10 (ms; scan MB/s of first_dim_core; ref h/N0)")
for c,p,h in scales:
    for label,root,sub in (("old default",old,"core-matrix"),("new 2gb",new,"core-matrix"),("new h10",new,"core-matrix-standard-h10")):
        r=standard(root,c,sub)
        if r: print(f"  {c:6s} {label:12s} server {r[0]:8.1f}  throughput {r[1]/r[0]/1e3:7.0f} MB/s  first_dim_core {r[5]:7.1f} ({r[1]/r[5]/1e3:6.0f} MB/s)  ref h={r[2]} N0={r[3]} ok={r[4]}")

print("\n== Throughput table, MB/s = logical bytes / server ms per path (headline metric)")
tp=lambda B,ms: B/ms/1e3
hdr=["256mib","512mib","1gib","2gib","4gib"]; table={}
def put(k,i,v): table.setdefault(k,[None]*5); table[k][i]=v
for i,(c,p,h) in enumerate(scales):
    B=(2**(h+1)-1)*32
    pr=primary(new,p)
    if pr: put("Ours h1 g8/P3 (new binary)",i,tp(B,min(v[0] for v in pr.values())))
    for label,root,sub in (("Standard h10",new,"core-matrix-standard-h10"),("Standard h10",new,"core-matrix"),("Standard h10",old,"core-matrix")):
        r=standard(root,c,sub)
        if r and (r[2]==10 or root is old and c in ("1gb","4gb")) and table.get("Standard h10",[None]*5)[i] is None: put("Standard h10",i,tp(r[1],r[0]))
    for name,fn in (("Layerwise tuned",lambda r: merkle(r,c,"layerwise_tuned")),("Flat",lambda r: merkle(r,c,"flat"))):
        r=fn(new) or fn(old)
        if r: put(name,i,tp(r[1],r[0]))
    r=cuckoo(new,c) or cuckoo(old,c)
    if r: put("Cuckoo batch tuned",i,tp(r[1],r[0]))
    r=treepir(new,c) or treepir(old,c)
    if r: put("TreePIR",i,tp(B,r[0]))
    for k,v in list(ext_old.items())+list(ext_new.items()):
        sc,mode=k.split(":")
        if sc==p: put({"spirit_rc":"SPIRIT+RC","respire":"Respire"}[mode],i,v[1])
print("| scheme | "+" | ".join(hdr)+" |"); print("|---|"+"---:|"*5)
for k in ("Ours h1 g8/P3 (new binary)","Standard h10","Layerwise tuned","SPIRIT+RC","Cuckoo batch tuned","TreePIR","Flat","Respire"):
    v=table.get(k,[None]*5); print("| "+k+" | "+" | ".join(f"{x:.0f}" if x else "-" for x in v)+" |")

print("\n== 2 GiB baselines (new root)")
for name,fn in (("layerwise_tuned",lambda: merkle(new,"2gb","layerwise_tuned")),("flat",lambda: merkle(new,"2gb","flat")),("cuckoo_tuned",lambda: cuckoo(new,"2gb")),("treepir",lambda: treepir(new,"2gb"))):
    print(f"  {name:16s} {fn()}")
for k,v in ext_new.items(): print(f"  {k:16s} server {v[0]:9.1f} ms  {v[1]:7.0f} MB/s")

print("\n== status files")
for f in ("metadata/status","metadata/phase-exits.tsv","metadata/launcher.log"):
    p=new/f
    if p.exists(): print(f"  {f}: {p.read_text().strip().replace(chr(10),' | ')}")
