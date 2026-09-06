#!/usr/bin/env bash
# Reuse the completed Wisconsin core binary for directly comparable entry runs.
set -euo pipefail
RUN_ROOT="${1:?usage: run_wisconsin_entry_sizes.sh RUN_ROOT CORE_RUN_ROOT}"
CORE_ROOT="${2:?missing completed core run root}"
SOURCE="$RUN_ROOT/source"
META="$RUN_ROOT/metadata"
BIN="$CORE_ROOT/source/build-native/Onion-PIR"
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
date -u +%FT%TZ > "$META/started-utc.txt"
cp "$CORE_ROOT/metadata/source-commit.txt" "$META/binary-source-commit.txt"
printf '%s\n' "$BIN" > "$META/binary-path.txt"
SOURCE_COMMIT="$(cat "$META/binary-source-commit.txt")"
export OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1 MKL_NUM_THREADS=1
export SUPERRECORD_MEMORY_BYTES=85899345920
export PYTHONUNBUFFERED=1
cd "$SOURCE"
echo validating > "$META/status"
numactl --physcpubind=0 --interleave=all python3 scripts/check_entry_size_smoke.py \
  --binary "$BIN" --source-commit "$SOURCE_COMMIT" \
  --output-dir "$RUN_ROOT/outputs/smoke" > "$META/smoke.log" 2>&1
echo running > "$META/status"
numactl --physcpubind=0 --interleave=all python3 scripts/run_entry_size_sweep.py \
  --binary "$BIN" --source-commit "$SOURCE_COMMIT" --entries 16,48,64,128 \
  --output-dir "$RUN_ROOT/outputs/formal" > "$META/formal.log" 2>&1
