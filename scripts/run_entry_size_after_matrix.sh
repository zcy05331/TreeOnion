#!/usr/bin/env bash
# Serialize the new entry-size study after the already-running 8 GiB matrix.
set -euo pipefail
RUN_ROOT="${1:?usage: run_entry_size_after_matrix.sh NEW_RUN_ROOT PREDECESSOR_PID OLD_RUN_ROOT}"
PREDECESSOR_PID="${2:?missing predecessor PID}"
OLD_ROOT="${3:?missing predecessor run root}"
SOURCE="$RUN_ROOT/source"
META="$RUN_ROOT/metadata"
BIN="$SOURCE/build-native/Onion-PIR"
mkdir -p "$META" "$RUN_ROOT/outputs"
printf '%s\n' "$$" > "$META/queue-controller.pid"
finish() {
  local code=$?
  printf '%s\n' "$code" > "$META/queue.exit-code.txt"
  if [[ "$code" == 0 ]]; then echo complete > "$META/queue.status";
  else echo failed > "$META/queue.status"; fi
}
trap finish EXIT
echo waiting_for_predecessor > "$META/queue.status"
python3 - "$PREDECESSOR_PID" <<'PY'
import pathlib,sys,time
status = pathlib.Path('/proc') / sys.argv[1] / 'status'
while status.exists():
    try:
        if '\nState:\tZ' in status.read_text():
            break
    except FileNotFoundError:
        break
    time.sleep(45)
PY
if [[ -f "$OLD_ROOT/metadata/matrix.status" ]]; then
  cp "$OLD_ROOT/metadata/matrix.status" "$META/predecessor-final.status"
fi
export OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1 MKL_NUM_THREADS=1
cd "$SOURCE"
echo building > "$META/queue.status"
cmake -S . -B build-native -DCMAKE_BUILD_TYPE=Benchmark \
  -DACTIVE_CONFIG=CONFIG_N2048_K1_COMP \
  -DHEXL_DIR="$RUN_ROOT/deps/hexl-install/lib64/cmake/hexl-1.2.6" \
  > "$META/native-configure.log" 2>&1
cmake --build build-native -j 8 > "$META/native-build.log" 2>&1
echo validating > "$META/queue.status"
numactl --physcpubind=0 --interleave=all bash scripts/run_regression.sh "$BIN" \
  > "$META/native-regression.log" 2>&1
python3 -m unittest discover -s scripts/tests > "$META/python-tests.log" 2>&1
SOURCE_COMMIT="$(cat "$META/source-commit.txt")"
numactl --physcpubind=0 --interleave=all python3 scripts/check_entry_size_smoke.py \
  --binary "$BIN" --source-commit "$SOURCE_COMMIT" \
  --output-dir "$RUN_ROOT/outputs/native-smoke" > "$META/native-smoke.log" 2>&1
echo running > "$META/queue.status"
numactl --physcpubind=0 --interleave=all python3 scripts/run_entry_size_sweep.py \
  --binary "$BIN" --source-commit "$SOURCE_COMMIT" \
  --reuse-32b "$OLD_ROOT/outputs/profiling/1gb/generic.json" \
  --output-dir "$RUN_ROOT/outputs/entry-size-1gb" > "$META/entry-size-run.log" 2>&1
