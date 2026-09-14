#!/usr/bin/env bash
set -euo pipefail
TASK_BASE=/users/zcy05331/generic-superrecord-bench-20260904
TASK_ROOT="$TASK_BASE/revision-fb79387"
cd "$TASK_ROOT/source"
export OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1 MKL_NUM_THREADS=1
export ONIONPIR_BENCH_COMMIT=fb793878713fc017e228af15750a4d247c2ea6d1
export ONIONPIR_BENCH_BRANCH=codex/generic-superrecord
OUT="$TASK_BASE/outputs/profiling/1gb/tuning/cuckoo"
mkdir -p "$OUT"
# Preserve the current preflight in RAM and exclude concurrent memory traffic
# from the latency pilots. The wrapper remains alive for the queued handoff.
python3 - "$TASK_ROOT" "$TASK_BASE" <<'PY'
import json,os,signal,sys,time
from pathlib import Path
from datetime import datetime,timezone
root,base=map(Path,sys.argv[1:])
pid=15163
args=Path(f'/proc/{pid}/cmdline').read_bytes().replace(b'\0',b' ').decode()
assert 'revision-22adeee/source/build-native/Onion-PIR --test tree_superrecord_bench' in args
log=(base/'outputs/profiling/1gb/generic.log').read_text()
assert 'completed measured block' not in log
entry={'pid':pid,'started_utc':datetime.now(timezone.utc).isoformat(),'started_monotonic':time.monotonic(),
       'preparing':next((line for line in reversed(log.splitlines()) if line.startswith('preparing [')),''),
       'scope':'generic preflight wall-clock includes this pause; online timing has not started'}
(root/'metadata/generic-pause-for-pilots.json').write_text(json.dumps(entry,indent=2)+'\n')
os.kill(pid,signal.SIGSTOP)
for _ in range(20):
    state=Path(f'/proc/{pid}/stat').read_text().rsplit(')',1)[1].split()[0]
    if state in ('T','t'):break
    time.sleep(.05)
else:
    os.kill(pid,signal.SIGCONT)
    raise RuntimeError('generic process did not pause; pilots will not run')
PY
resume_generic() {
  local result="$?"
  kill -CONT 15163 2>/dev/null || true
  python3 - "$TASK_ROOT" "$result" <<'PY'
import json,sys,time
from pathlib import Path
from datetime import datetime,timezone
p=Path(sys.argv[1])/'metadata/generic-pause-for-pilots.json'
d=json.loads(p.read_text());d['resumed_utc']=datetime.now(timezone.utc).isoformat()
d['paused_seconds']=time.monotonic()-d['started_monotonic'];d['pilot_exit_code']=int(sys.argv[2])
p.write_text(json.dumps(d,indent=2)+'\n')
PY
}
trap resume_generic EXIT
for HEIGHT in 7 8 9 10; do
  printf '1gb cuckoo pilot height=%s\n' "$HEIGHT" > "$TASK_ROOT/metadata/pilot-current-job.txt"
  CUCKOO_LAYOUT_HEIGHT="$HEIGHT" BENCHMARK_OUTPUT_JSON="$OUT/h$HEIGHT.json" \
    python3 scripts/read_process_peak_memory.py --log "$OUT/h$HEIGHT.log" --output "$OUT/h$HEIGHT-process.json" \
    -- numactl --physcpubind=0 --interleave=all ./build-native/Onion-PIR \
    --test cuckoo_bench --leaf-count 16777216 --experiments 16 --warmup 3 --trial-seed 7
  python3 scripts/summarize_cloudlab_profiles.py --validate "$OUT/h$HEIGHT.json" --trials 16 --leaf-count 16777216
done
python3 scripts/select_cuckoo_layout.py --output "$OUT/selection.json" --expected-trials 16 --expected-warmups 3 --leaf-count 16777216 "$OUT/h7.json" "$OUT/h8.json" "$OUT/h9.json" "$OUT/h10.json" > "$TASK_ROOT/metadata/pilot-chosen-height.txt"
echo complete > "$TASK_ROOT/metadata/pilot.status"
