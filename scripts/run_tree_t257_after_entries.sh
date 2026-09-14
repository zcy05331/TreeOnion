#!/usr/bin/env bash
# Called only after the local t257 comparison passes. Build after entry runs end.
set -euo pipefail
RUN_ROOT="${1:?usage: run_tree_t257_after_entries.sh RUN_ROOT ENTRY_RUN_ROOT CORE_RUN_ROOT}"
ENTRY_ROOT="${2:?missing entry run root}"
CORE_ROOT="${3:?missing core run root}"
META="$RUN_ROOT/metadata"
SOURCE="$RUN_ROOT/source"
mkdir -p "$META" "$RUN_ROOT/outputs"
echo "$$" > "$META/controller.pid"
finish() {
  local code=$?
  echo "$code" > "$META/exit-code.txt"
  if [[ "$code" == 0 ]]; then echo complete > "$META/status";
  else echo failed > "$META/status"; fi
}
trap finish EXIT
echo waiting_for_entries > "$META/status"
python3 - "$ENTRY_ROOT" <<'PY'
from pathlib import Path
import sys, time
root = Path(sys.argv[1]) / 'metadata'
pid = int((root / 'controller.pid').read_text())
process = Path('/proc') / str(pid) / 'status'
while process.exists():
    try:
        if '\nState:\tZ' in process.read_text():
            break
    except FileNotFoundError:
        break
    time.sleep(45)
state = (root / 'status').read_text().strip()
if state not in ('complete', 'failed'):
    raise RuntimeError(f'entry controller ended without a terminal status: {state}')
print(f'Entry predecessor ended: {state}', flush=True)
PY
cp "$ENTRY_ROOT/metadata/status" "$META/entry-predecessor.status"
export OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1 MKL_NUM_THREADS=1
export PYTHONUNBUFFERED=1
cd "$SOURCE"
echo building > "$META/status"
for VARIANT in baseline t257; do
  CONFIG=CONFIG_N2048_K1_COMP
  if [[ "$VARIANT" == t257 ]]; then CONFIG=CONFIG_N2048_K1_COMP_T257; fi
  cmake -S . -B "build-$VARIANT" -DCMAKE_BUILD_TYPE=Benchmark \
    -DACTIVE_CONFIG="$CONFIG" \
    -DHEXL_DIR="$CORE_ROOT/deps/hexl-install/lib64/cmake/hexl-1.2.6" \
    > "$META/configure-$VARIANT.log" 2>&1
  cmake --build "build-$VARIANT" -j4 > "$META/build-$VARIANT.log" 2>&1
  for TEST in hexl_ntt tree_g32; do
    numactl --physcpubind=0 --interleave=all "build-$VARIANT/Onion-PIR" --test "$TEST" \
      > "$META/$VARIANT-$TEST.log" 2>&1
  done
done
echo validating > "$META/status"
numactl --physcpubind=0 --interleave=all python3 scripts/run_tree_t257_comparison.py \
  --baseline-binary build-baseline/Onion-PIR --candidate-binary build-t257/Onion-PIR \
  --leaf-count 4096 --experiments 3 --warmup 1 \
  --output-dir "$RUN_ROOT/outputs/smoke" > "$META/smoke.log" 2>&1
echo running > "$META/status"
numactl --physcpubind=0 --interleave=all python3 scripts/run_tree_t257_comparison.py \
  --baseline-binary build-baseline/Onion-PIR --candidate-binary build-t257/Onion-PIR \
  --leaf-count 16777216 --experiments 100 --warmup 3 \
  --output-dir "$RUN_ROOT/outputs/formal-1gb" > "$META/formal.log" 2>&1
