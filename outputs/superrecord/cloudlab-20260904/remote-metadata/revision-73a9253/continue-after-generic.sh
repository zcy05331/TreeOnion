#!/usr/bin/env bash
set -euo pipefail
TASK_BASE=/users/zcy05331/generic-superrecord-bench-20260904
TASK_ROOT="$TASK_BASE/revision-73a9253"
echo waiting_for_existing_generic > "$TASK_ROOT/metadata/matrix.status"
printf '%s\n' '1gb generic (revision-22adeee running; aligned queue waiting)' > "$TASK_ROOT/metadata/current-job.txt"
while true; do
  JOB_STATE=$(ps -p 15162 -o stat= || true)
  [[ -z "$JOB_STATE" || "$JOB_STATE" == Z* ]] && break
  sleep 10
done
# The old controller was paused while its current child continued. End only
# that controller before it can dispatch another old-version job.
kill -TERM 14403 2>/dev/null || true
kill -CONT 14403 2>/dev/null || true
python3 - "$TASK_BASE" <<'PY'
import json,sys,shutil
from pathlib import Path
base=Path(sys.argv[1]); source=base/'outputs/profiling/1gb'
archive=base/'outputs/controls/cuckoo-unaligned-reference-22adeee/1gb'
archive.mkdir(parents=True,exist_ok=True)
for suffix in ['.json','.log','-process.json']:
    p=source/('cuckoo'+suffix)
    if p.exists():shutil.move(str(p),str(archive/p.name))
proc=source/'generic-process.json'
if not proc.exists() or json.loads(proc.read_text())['exit_code']!=0:
    failure=base/'outputs/failures/generic-revision-22adeee'
    failure.mkdir(parents=True,exist_ok=True)
    for suffix in ['.json','.log','-process.json']:
        p=source/('generic'+suffix)
        if p.exists():shutil.move(str(p),str(failure/p.name))
PY
printf '%s\n' "$TASK_ROOT" > "$TASK_BASE/metadata/active-run-root.txt"
printf '%s\n' generic-profile-73a9253 > "$TASK_BASE/metadata/active-session.txt"
date -u +%FT%TZ > "$TASK_ROOT/metadata/handoff-complete-at.txt"
exec bash "$TASK_ROOT/source/scripts/run_cloudlab_profile_matrix.sh" "$TASK_ROOT" > "$TASK_ROOT/metadata/matrix-controller.log" 2>&1
