#!/usr/bin/env bash
set -euo pipefail
RUN_ROOT=/users/zcy05331/generic-entry-recovery-clemson-20260907
SOURCE=/users/zcy05331/tree-t257-clemson-20260907/source
META="$RUN_ROOT/metadata"
BIN="$SOURCE/build-baseline/Onion-PIR"
mkdir -p "$META" "$RUN_ROOT/outputs"
echo "$$" > "$META/controller.pid"
finish() {
  local code=$?
  echo "$code" > "$META/exit-code.txt"
  if [[ "$code" == 0 ]]; then echo complete > "$META/status";
  else echo failed > "$META/status"; fi
  date -u +%FT%TZ > "$META/finished-utc.txt"
}
trap finish EXIT
export OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1 MKL_NUM_THREADS=1 PYTHONUNBUFFERED=1
export SUPERRECORD_MEMORY_BYTES=85899345920
cd "$SOURCE"
echo validating > "$META/status"
numactl --physcpubind=0 --interleave=all python3 scripts/check_entry_size_smoke.py \
  --binary "$BIN" --source-commit 932a4edb0efc91b2eac3a43d9d0e541cd0d15dee \
  --output-dir "$RUN_ROOT/outputs/smoke" > "$META/smoke.log" 2>&1
echo running > "$META/status"
numactl --physcpubind=0 --interleave=all python3 scripts/run_entry_size_sweep.py \
  --binary "$BIN" --source-commit 932a4edb0efc91b2eac3a43d9d0e541cd0d15dee \
  --entries 48,128 --output-dir "$RUN_ROOT/outputs/formal" > "$META/formal.log" 2>&1
