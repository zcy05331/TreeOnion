#!/usr/bin/env python3
"""Emit RESULTS-ADDENDUM.md from the campaign root, the addendum root and the phase-2 root (all synced locally)."""
import json, statistics as st, glob, sys
from pathlib import Path
OLD, NEW, NEW2, OUT = (Path(a) for a in sys.argv[1:5])
sizes=[("256mb","256mib",22),("512mb","512mib",23),("1gb","1gib",24),("2gb","2gib",25),("4gb","4gib",26)]
def mean(xs): return st.mean(xs)
def load(p):
    p=Path(p)
    return json.load(open(p)) if p.exists() else None
def logical(h): return (2**(h+1)-1)*32
def tp(B,ms): return B/ms/1e3

# ---- per-row records: (server_ms, MB/s, query B, response B, keys B, note)
T={}
def put(scheme,i,ms,B,q,r,k,note=""): T.setdefault(scheme,[None]*5); T[scheme][i]=dict(ms=ms,tp=tp(B,ms),q=q,r=r,k=k,note=note)
for i,(c,p,h) in enumerate(sizes):
    B=logical(h)
    # ours: new binary, best layout
    best=None
    for f in glob.glob(str(NEW/f"outputs/primary/{p}/formal/*.json")):
        if f.endswith("-process.json"): continue
        d=load(f); ms=mean(x["server_total_ms"] for x in d["samples"])
        if best is None or ms<best[0]: best=(ms,d)
    ms,d=best; cm=d["communication"]
    put("Ours h1 g8/P3",i,ms,B,cm["modeled_query_bytes_per_trial"],cm["actual_response_bytes_per_trial"],cm["modeled_helper_key_bytes"],f"N0 {d['metadata']['N0']}, {d['metadata']['query_layout']}")
    # standard: h10 everywhere (small sizes from the h10 rerun; 1/4 GiB same-day rerun; 2 GiB addendum)
    src = NEW/f"outputs/core-matrix-standard-h10/{c}/standard.json" if c in ("256mb","512mb") else (NEW/f"outputs/core-matrix/{c}/standard.json" if c=="2gb" else NEW/f"outputs/core-matrix-sameday/{c}/standard.json")
    d=load(src); cs=d["cases"][0]
    put("Standard OnionPIR (single block)",i,cs["server_compute_ms"],cs["raw_dataset_bytes"],cs["online_query_bytes_modeled"],cs["online_response_bytes_actual"],cs["helper_key_bytes_modeled"],f"h{d['workload'].get('reference_expansion_height')} N0 {d['workload'].get('reference_fst_dim_sz')}")
    # merkle baselines: small sizes from phase-2 h10 root when present, else campaign/addendum
    def merkle(name, fn, sub_new2="core-matrix-h10"):
        cand=[NEW2/f"outputs/{sub_new2}/{c}/{fn}.json"] if c in ("256mb","512mb") else []
        cand+=[NEW/f"outputs/core-matrix/{c}/{fn}.json", OLD/f"outputs/core-matrix/{c}/{fn}.json"]
        for f in cand:
            d=load(f)
            if d: return d,f
        return None,None
    for name,fn in (("Layerwise tuned","layerwise_tuned"),("Flat","flat")):
        d,f=merkle(name,fn)
        if d:
            cs=d["cases"][0]; h=d["workload"].get("reference_expansion_height") or (10 if d["workload"]["leaf_count"] >= (1<<24) else 7)
            put(name,i,cs["server_compute_ms"],cs["raw_dataset_bytes"],cs["online_query_bytes_modeled"],cs["online_response_bytes_actual"],cs["helper_key_bytes_modeled"],f"{cs['pir_call_count']} calls, h{h}")
    d,f=merkle("cuckoo","cuckoo_tuned")
    if d:
        ms=mean(x["server_total_ms"] for x in d["samples"]); cm=d["communication"]
        put("Cuckoo batch tuned",i,ms,d["actual_logical_bytes"],cm["modeled_query_bytes"],cm["actual_response_bytes"],cm["modeled_helper_key_bytes"],f"{d['num_buckets']} buckets, h{d['requested_expansion_height']}")
    # merged layerwise: phase-2 (small sizes in core-matrix-h10, others in core-matrix-merged)
    for f in (NEW2/f"outputs/core-matrix-h10/{c}/layerwise_merged_tuned.json", NEW2/f"outputs/core-matrix-merged/{c}/layerwise_merged_tuned.json"):
        d=load(f)
        if d:
            cs=d["cases"][0]; put("Layerwise + response merging (64-slot strided)",i,cs["server_compute_ms"],cs["raw_dataset_bytes"],cs["online_query_bytes_modeled"],cs["online_response_bytes_actual"],cs["helper_key_bytes_modeled"],f"{cs['pir_call_count']} queries, resp {cs['online_response_bytes_actual']} B")
            break
    # treepir
    root = NEW if c=="2gb" else OLD
    d=load(root/f"outputs/treepir-backend-tuning/{c}/formal.json")
    if d:
        res=[r for r in d["results"] if r["phase"]=="measured"]
        put("TreePIR",i,d["measured_summary"]["server_ms_mean"],B,mean(r["actual_query_payload_bytes_unseeded"] for r in res),d["measured_summary"]["actual_response_payload_bytes_mean"],d["actual_helper_key_bytes_uncompressed"],d["backend_label"])
for root in (OLD,NEW):
    s=load(root/"outputs/external-matrix/summary.json")
    if not s: continue
    for r in s["summary_rows"]:
        sc,mode=r["scale_mode"].split(":"); i=[x[1] for x in sizes].index(sc)
        name={"spirit_rc":"SPIRIT+RC","respire":"Respire"}[mode]
        q,keys=r["query_bytes"],None
        if mode=="spirit_rc":
            # The adapter bills reusable key material to every query; split it out.
            det=load(Path(r["raw"].replace("/users/zcy05331/tree-h1-final-20260920T2105Z",str(OLD)).replace("/users/zcy05331/tree-h1-addendum-20260921T2057Z",str(NEW))))
            if det:
                t=det["samples"][0]["details"]
                keys=t["query_galois_key_bytes"]+t["query_rc_galois_key_bytes"]
                q=t["query_vector_bytes"]+t["query_packed_secret_bytes"]+t["query_rc_selector_bytes"]
        elif mode=="respire":
            keys=(11*24+2*4+8*2)*14336  # crate params_public_param_size dominant term
        put(name,i,r["server_ms"],logical(sizes[i][2]),q,r["response_bytes"],keys,"")

order=["Ours h1 g8/P3","Standard OnionPIR (single block)","Layerwise tuned","Layerwise + response merging (64-slot strided)","SPIRIT+RC","Cuckoo batch tuned","TreePIR","Flat","Respire"]
hdr="| 256 MiB | 512 MiB | 1 GiB | 2 GiB | 4 GiB |"
def cell(v,key,fmt):
    return "—" if v is None or v.get(key) is None else fmt(v[key])
def kb(x): return f"{x/1024:.1f} KB" if x < 1024*1024 else f"{x/1024/1024:.2f} MB"
lines=[]
lines.append("# Utah addendum results (Sept 21-22 2026)\n")
lines.append("Host er031.utah.cloudlab.us (Xeon Gold 5512U, 125 GiB), CPU 0 pinned, memory node 0. Fifty measured queries per row (eight for Cuckoo, TreePIR and Flat), three warmups, seed 7. Logical database = full tree, (2^(H+1)-1) x 32 B.\n")
lines.append("## Throughput (MB/s = logical bytes / mean server time per complete path)\n")
lines.append("| Scheme "+hdr); lines.append("|---|---:|---:|---:|---:|---:|")
for k in order:
    v=T.get(k,[None]*5); lines.append(f"| {k} | "+" | ".join(cell(x,'tp',lambda t: f'{t:.0f}') for x in v)+" |")
# marginal throughput between adjacent sizes: separates fixed cost from scan efficiency
lines.append("\n## Marginal throughput (MB/s between adjacent sizes, and the implied ceiling)\n")
lines.append("| Scheme | 256-512 MiB | 512 MiB-1 GiB | 1-2 GiB | 2-4 GiB | ceiling |"); lines.append("|---|---:|---:|---:|---:|---:|")
GiB=2**30; szs=[0.25,0.5,1.0,2.0,4.0]
for k in order:
    v=T.get(k,[None]*5); cells=[]; last=None
    for i in range(4):
        a,b=v[i],v[i+1]
        if not a or not b: cells.append("—"); continue
        t=(szs[i+1]-szs[i])*GiB/(b["ms"]-a["ms"])/1e3; cells.append(f"{t:.0f}"); last=t
    lines.append(f"| {k} | "+" | ".join(cells)+f" | {f'{last:.0f}' if last else '—'} |")
lines.append("\nA scheme's own curve is flat in this table; a jump means the row changed backend (TreePIR switches from vbpir to sealpir_d2 at 4 GiB). SPIRIT+RC has 3.2 times our marginal throughput and about 5.7 times our fixed cost, which is why it crosses over near 1.4 GiB.\n")

lines.append("\n## Server time per path (ms)\n")
lines.append("| Scheme "+hdr); lines.append("|---|---:|---:|---:|---:|---:|")
for k in order:
    v=T.get(k,[None]*5); lines.append(f"| {k} | "+" | ".join(cell(x,'ms',lambda t: f'{t:.1f}') for x in v)+" |")
lines.append("\n## Per-stage throughput at 1 GiB (MB/s = logical bytes / stage time)\n")
lines.append("| Stage | Ours | Standard | Layerwise | Cuckoo | Respire |"); lines.append("|---|---:|---:|---:|---:|---:|")
for row in (('query expansion',14434,13071,2845,676,103),('first dimension',1460,1669,1474,454,181),('RGSW fold / other dimensions',19544,12074,7574,2064,121),('whole path',1120,1317,859,240,42)):
    lines.append("| "+row[0]+" | "+" | ".join(str(x) for x in row[1:])+" |")
lines.append("\nOurs and Standard are capped by the first dimension alone; every other stage runs above 12 GB/s. Cuckoo pays expansion and first dimension once per bucket (37 buckets at 1 GiB). Respire runs every stage below 200 MB/s, with expansion and fold slower than its own first dimension, because each of its 49 buckets pays a full expansion and a full 256-to-1 fold.\n")

lines.append("\n## What response merging costs Layerwise\n")
lines.append("| Size | Layerwise MB/s | with merging MB/s | ratio | scanned bytes ratio | response |"); lines.append("|---|---:|---:|---:|---:|---:|")
for c,(lab,) in zip(("256mb","512mb","1gb","2gb","4gb"),[(x,) for x in ("256 MiB","512 MiB","1 GiB","2 GiB","4 GiB")]):
    a=T.get("Layerwise tuned"); b=T.get("Layerwise + response merging (64-slot strided)")
    i=["256mb","512mb","1gb","2gb","4gb"].index(c)
    if a[i] and b[i]:
        lines.append(f"| {lab} | {a[i]['tp']:.0f} | {b[i]['tp']:.0f} | {b[i]['tp']/a[i]['tp']:.2f} | 1.49 | {b[i]['r']/1024:.1f} KB |")
lines.append("\nThe throughput ratio tracks the inverse of the scanned-byte ratio, so the merge itself is nearly free and the whole cost is the strided layout holding 64 instead of 96 nodes per plaintext. Marginal throughput falls from 1421 to 924 MB/s.\n")

lines.append("\n## Communication\n")
lines.append("Split into what a client uploads once per session (offline) and what one path retrieval costs (online). SPIRIT+RC's adapter charges its reusable key material to every query; it is separated here using the adapter's own per-sample breakdown.\n")
lines.append("### Offline, once per session\n")
lines.append("| Scheme | 256 MiB | 512 MiB | 1 GiB | 2 GiB | 4 GiB | Kind |"); lines.append("|---|---:|---:|---:|---:|---:|---|")
offline_kind = {
  "Ours h1 g8/P3": "BV Galois + RGSW(s) keys, seeded model",
  "Standard OnionPIR (single block)": "BV Galois + RGSW(s) keys, seeded model",
  "Layerwise tuned": "same key bundle shared by every level",
  "Layerwise + response merging (64-slot strided)": "same key bundle, also covers the merge automorphisms",
  "Cuckoo batch tuned": "same key bundle shared by every bucket",
  "Flat": "same key bundle",
  "TreePIR": "GaloisKeys + RelinKeys, actual uncompressed bytes",
  "SPIRIT+RC": "SPIRIT Galois + RC selector/merge keys, actual bytes (see notes)",
  "Respire": "public parameters, crate formula (see notes)",
}
for k in order:
    v=T.get(k,[None]*5)
    lines.append(f"| {k} | "+" | ".join(cell(x,'k',kb) for x in v)+f" | {offline_kind.get(k,'')} |")
lines.append("\n### Online query, per path\n")
lines.append("| Scheme "+hdr); lines.append("|---|---:|---:|---:|---:|---:|")
for k in order:
    v=T.get(k,[None]*5); lines.append(f"| {k} | "+" | ".join(cell(x,'q',kb) for x in v)+" |")
lines.append("\n### Online response, per path\n")
lines.append("| Scheme "+hdr); lines.append("|---|---:|---:|---:|---:|---:|")
for k in order:
    v=T.get(k,[None]*5); lines.append(f"| {k} | "+" | ".join(cell(x,'r',kb) for x in v)+" |")
lines.append("\n### Online total, per path\n")
lines.append("| Scheme "+hdr); lines.append("|---|---:|---:|---:|---:|---:|")
for k in order:
    v=T.get(k,[None]*5)
    lines.append(f"| {k} | "+" | ".join("—" if x is None or x.get('q') is None or x.get('r') is None else kb(x['q']+x['r']) for x in v)+" |")
lines.append("""
Measurement kinds. Query bytes for Ours, Standard, Layerwise, Flat and Cuckoo are the seed-compressed protocol model, one 14.5 KB packed BFV query per PIR call; those schemes upload only keys offline and the server keeps its own preprocessed database. TreePIR and SPIRIT+RC report actual serialized bytes. Respire reports its parameter model. Response bytes are actual serialized bytes for every scheme.

SPIRIT+RC. The adapter counts the RC selector expansion keys, the merge Galois keys and SPIRIT's own Galois keys inside every query. Its per-sample breakdown separates them: the key material is 3.30 MB at every size (SPIRIT Galois 657 KB, RC selector keys 2.38 MB, RC merge Galois 401 KB) and is listed as offline above, leaving the LWE query vector plus the packed secret and the RC selector ciphertext as the online query. Two further offline terms are not in the table. The client hint that its SimplePIR-style first dimension needs has no serializer in the adapter, so it is unmeasured. Its public metadata, the PBC routing tables and bucket layouts, is 0.94, 1.88, 3.75 and 7.50 GiB at the four sizes, reported by the adapter as an in-memory model rather than a wire requirement because it is reconstructible from public seeds and the layout.

Respire. The adapter hardcodes zero offline bytes, but the crate's own `params_public_param_size` charges automorphism, RLWE-to-GSW and scalar-to-vector public parameters: (11 x 24 + 2 x 4 + 8 x 2) elements x 14336 B = 3.94 MiB, plus a compression term of a few hundred KB, independent of database size. That figure is derived from the formula, not measured here, and the crate notes its randomness compression is not actually implemented.

TreePIR. Its 4 GiB row uses the SealPIR backend because every VBPIR candidate ran out of memory, which is why its keys jump from 37.57 MB to 846.77 MB and its query and response jump with them.
""")
lines.append("## Provenance\n")
lines.append("Three run roots on the same host, same source lineage, one leaf schedule per size (seed 7):\n")
lines.append("- `tree-h1-final-20260920T2105Z` (commit eac95e8): TreePIR and the external baselines at 256 MiB, 512 MiB, 1 GiB, 4 GiB.")
lines.append("- `tree-h1-addendum-20260921T2057Z` (commit ae8ae66): every Ours row re-measured on one binary, the 2 GiB point, Standard re-measured at 256/512 MiB under the pinned geometry and re-measured same-day at 1/4 GiB, TreePIR and the external baselines at 2 GiB.")
lines.append("- `tree-h1-addendum2-20260922T0143Z` (commit 8e3e5f9): Layerwise, Cuckoo and Flat re-measured at 256/512 MiB under the pinned geometry, and the merged-response Layerwise variant at all five sizes.\n")
lines.append("Raw per-round JSON stays on the local sync of those roots and is not committed.\n")
lines.append("## Findings\n")
lines.append("**Reference geometry.** Below 2^24 leaves `make_benchmark_reference` picked the smallest feasible BFV expansion height (7), giving the flat database a 32- or 64-wide first dimension; from 2^24 leaves it picks height 10 (first dimension 512). Every Merkle-derived case inherited that envelope, so the small-size columns were not comparable with the large ones. Re-measured at height 10, the small sizes gain: Standard 348 to 985 MB/s at 256 MiB and 353 to 1163 at 512 MiB, Flat 16 to 45 and 15 to 52, Layerwise 407 to 503 and 380 to 666, Cuckoo 131 to 136 and 157 to 187 (its pilot range also widens from height 7 only to 7-10, and it now selects 8). First-dimension scan rate is 1.80-1.87 GB/s at every size afterwards.\n")
lines.append("**Layerwise still falls off at small sizes** even under one geometry (503 MB/s at 256 MiB against 1196 at 4 GiB): its H independent packed queries cost a fixed 148 ms of expansion plus 74 ms of selector completion at 256 MiB, against 63 ms of expansion total for the single packed tree query.\n")
lines.append("**Response merging applied to Layerwise** (strided 64-slot level plaintexts, one shared 6-bit gamma selector set, `mlwe_merge_residues`) cuts the response from one ciphertext per level to one ciphertext for the whole path: 180-224 KB down to 14.9 KB, within 36 percent of the tree scheme's 11.0 KB. Rotation and merge are cheap (13.6 and 2.0 ms at 256 MiB). The cost is the strided layout's 64 instead of 96 nodes per plaintext, which inflates the scanned database by 1.5x and costs 23-30 percent of throughput at every size. The tree scheme avoids that trade because its 3-plane codec reaches 88.9 percent coefficient utilization while keeping the stride.\n")
lines.append("**Respire, same RLWE-RGSW paradigm, 20x slower than our Cuckoo baseline** at 1 GiB. Its own stage timers: query expansion 10.5 s (41 percent), first dimension 5.9 s (23 percent), GSW fold 8.9 s (34 percent). Three causes, in order of size: its 49 cuckoo buckets each pay a full query expansion and a full 256-to-1 GSW fold (214 and 181 ms per bucket, against 43 and 14 ms per bucket for our Cuckoo baseline, and one 63 ms expansion for the tree scheme, which then serves all levels); its 4-bit plaintext modulus needs three times the coefficients per database byte, giving 0.55 GB/s of plaintext in the first dimension against 1.55 GB/s; and its AVX2 u64 kernels against HEXL AVX-512 u32 CRT kernels. What it buys is the smallest communication in the table: 8 KB responses, 75-206 KB queries, no helper keys.\n")
lines.append("**SPIRIT+RC crosses over near 1.4 GiB.** Its server time is 913 ms of fixed cost plus about 251 ms per GiB; ours is about 160 ms plus 800 ms per GiB. The fixed part is RC selector expansion (513 ms), hint query precompute (203 ms) and LWE-to-RLWE packing (138 ms); the marginal part is a plaintext-byte LWE matrix-vector product, which streams half the bytes we do at higher bandwidth. It pays for that with queries that grow to 15.4 MB, 38.8 s of client time per path, 76 GiB resident at 2 GiB (out of memory at 4 GiB), and a parameter line SEAL only accepts at `sec_level=none`.\n")
lines.append("**Scan-buffer reuse** (one scratch per response instead of two allocations per level and plane) was measured by alternating the old and new binaries on the same host: 1.0-1.5 percent of scan core at 1 GiB, about 2.4 percent at 4 GiB after excluding a host disturbance. Ours still scans 6-9 percent slower per byte than Standard on same-day rows (9.06-9.28 against 9.83-9.93 GB/s of physical bytes); that gap is unexplained. The 12.5 percent extra bytes from the g8/P3 layout remain the larger term.\n")
lines.append("## Row notes\n")
for k in order:
    v=T.get(k,[None]*5); notes=[f"{sizes[i][1]}: {x['note']}" for i,x in enumerate(v) if x and x.get("note")]
    if notes: lines.append(f"- {k}: "+"; ".join(notes))
Path(OUT).write_text("\n".join(lines)+"\n")
print("\n".join(lines[:4+len(order)+2]))
