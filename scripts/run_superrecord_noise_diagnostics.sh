#!/usr/bin/env bash
# Diagnostic-only observations of the six prepared layouts from the failed run.
set -euo pipefail
RUN_ROOT="${1:?usage: run_superrecord_noise_diagnostics.sh DIAGNOSTIC_ROOT ORIGINAL_RUN_ROOT}"
BASE="${2:?missing original run root}"
SOURCE="$RUN_ROOT/source"
META="$RUN_ROOT/metadata"
OUT="$RUN_ROOT/outputs"
mkdir -p "$META" "$OUT"
echo "$$" > "$META/controller.pid"
trap 'echo failed > "$META/status"' ERR
# The original matrix must remain held while this independent diagnosis uses CPU0.
python3 - "$BASE" <<'PY'
from pathlib import Path
import sys
base=Path(sys.argv[1])
pid=int((base/'revision-fb79387/metadata/matrix-controller.pid').read_text())
p=Path('/proc')/str(pid)
state=next(l.split()[1] for l in (p/'status').read_text().splitlines() if l.startswith('State:'))
if state != 'T': raise RuntimeError('Original core matrix is not held')
PY
export OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1 MKL_NUM_THREADS=1
cd "$SOURCE"
echo building > "$META/status"
cmake -S . -B build-native -DCMAKE_BUILD_TYPE=Benchmark \
  -DACTIVE_CONFIG=CONFIG_N2048_K1_COMP \
  -DHEXL_DIR="$BASE/deps/hexl-install/lib64/cmake/hexl-1.2.6" > "$META/build.log" 2>&1
cmake --build build-native -j8 >> "$META/build.log" 2>&1
BIN="$SOURCE/build-native/Onion-PIR"
echo native_smoke > "$META/status"
SUPERRECORD_NOISE_PROBE_OUTPUT="$OUT/smoke.json" \
  numactl --physcpubind=0 --interleave=all "$BIN" --test tree_superrecord_noise_probe > "$OUT/smoke.log" 2>&1
for index in 0 1 2 3 4 5; do
  echo "running_shape_$index" > "$META/status"
  # Margin/decode failure is an observation; retain JSON and inspect every shape.
  set +e
  SUPERRECORD_NOISE_PROBE_INDEX="$index" SUPERRECORD_NOISE_PROBE_OUTPUT="$OUT/shape$index.json" \
    python3 scripts/read_process_peak_memory.py \
    --log "$OUT/shape$index.log" --output "$OUT/shape$index-process.json" \
    -- numactl --physcpubind=0 --interleave=all "$BIN" --test tree_superrecord_noise_probe
  code=$?
  set -e
  echo "$code" > "$OUT/shape$index.exit-code.txt"
done
python3 - "$OUT" "$META" <<'PY'
from pathlib import Path
import json,sys
out,meta=map(Path,sys.argv[1:])
rows=[]
for index in range(6):
    p=out/f'shape{index}.json'
    if not p.exists():
        rows.append({'index':index,'status':'missing_diagnostic_output'})
        continue
    d=json.loads(p.read_text())
    assert d['diagnostic_only'] and not d['formal_benchmark_record'] and not d['validate_gate_certificate']
    assert d['fixed_threshold_bits']==1
    assert d['shape']['leaf_count']==134217728
    assert [x['leaf'] for x in d['queries']]==[15830119,113195322,40120694]
    rows.append({'index':index,'status':'observed','shape':d['shape'],'all_correct':d['all_correct'],
                 'all_margins_ok':d['all_margins_ok'],'queries':[
       {k:q[k] for k in ['leaf','correct_path','correct_selected_bundle','min_final_budget_bits','max_noise',
                        'max_used_phase_residual','max_unused_phase_residual']}
       for q in d['queries']]})
(meta/'summary.json').write_text(json.dumps({'diagnostic_only':True,'fresh_replication':True,'shapes':rows},indent=2)+'\n')
missing=any(r['status']!='observed' for r in rows)
failures=any(not r.get('all_correct',False) or not r.get('all_margins_ok',False) for r in rows)
(meta/'status').write_text(('failed' if missing else 'complete_with_findings' if failures else 'complete_no_failure_reproduced')+'\n')
PY
