#!/usr/bin/env bash
# 7 historical controls + generic/h1 sweep + TreePIR, each at 1/8 GiB.
set -uo pipefail
RUN_ROOT="${1:?usage: run_cloudlab_profile_matrix.sh RUN_ROOT}"
SOURCE="$RUN_ROOT/source"
BIN="$SOURCE/build-native/Onion-PIR"
TREEPIR="$RUN_ROOT/deps/TreePIR/VBPIR_TreePIR/build/bin/vbpir_treepir"
TRIALS="${TRIALS:-100}"
FLAT_TRIALS="${FLAT_TRIALS:-16}"
WARMUPS="${WARMUPS:-3}"
TRIAL_SEED="${TRIAL_SEED:-7}"
mkdir -p "$RUN_ROOT/outputs/profiling" "$RUN_ROOT/metadata"
cd "$SOURCE"
export OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1 MKL_NUM_THREADS=1
export ONIONPIR_BENCH_COMMIT="$(cat "$RUN_ROOT/metadata/source-commit.txt")"
export ONIONPIR_BENCH_BRANCH=codex/generic-superrecord
export ONIONPIR_BENCH_CPU="$(lscpu | sed -n 's/^Model name:[[:space:]]*//p')"
export ONIONPIR_BENCH_OS="$(uname -sr)"
export ONIONPIR_BENCH_COMPILER="$(g++ --version | head -n1)"
export ONIONPIR_BENCH_CMAKE="$(cmake --version | head -n1)"
export ONIONPIR_BENCH_HEXL_PATH="$RUN_ROOT/deps/hexl-install/lib64/cmake/hexl-1.2.6"
export ONIONPIR_BENCH_PROCESS_ARCH=x86_64
export SUPERRECORD_COMMIT="$ONIONPIR_BENCH_COMMIT"
STATUS_FILE="$RUN_ROOT/outputs/profiling/status.tsv"
[[ -f "$STATUS_FILE" ]] || printf 'scale\tcase\tstatus\texit_code\tutc\n' > "$STATUS_FILE"
printf '%s\n' "$$" > "$RUN_ROOT/metadata/matrix-controller.pid"
FAILURES=0
run_case() {
  local scale="$1" name="$2"; shift 2
  local folder="$RUN_ROOT/outputs/profiling/$scale" code
  local expected_trials="${CASE_TRIALS:-$TRIALS}"
  mkdir -p "$folder"
  # Resume a completed process only after its requested sample count validates.
  if [[ -f "$folder/$name.json" && -f "$folder/$name-process.json" ]]; then
    if python3 - "$folder/$name-process.json" <<'PY'
import json,sys
from pathlib import Path
sys.exit(0 if json.loads(Path(sys.argv[1]).read_text())['exit_code'] == 0 else 1)
PY
    then
      if python3 "$SOURCE/scripts/summarize_cloudlab_profiles.py" --validate "$folder/$name.json" --trials "$expected_trials" --leaf-count "$LEAVES"; then
        printf '%s\t%s\tcomplete\t0\t%s\n' "$scale" "$name" "$(date -u +%FT%TZ)" >> "$STATUS_FILE"
        return 0
      fi
    fi
  fi
  printf '%s %s\n' "$scale" "$name" > "$RUN_ROOT/metadata/current-job.txt"
  printf '%s\t%s\trunning\t\t%s\n' "$scale" "$name" "$(date -u +%FT%TZ)" >> "$STATUS_FILE"
  python3 "$SOURCE/scripts/read_process_peak_memory.py" \
    --log "$folder/$name.log" --output "$folder/$name-process.json" \
    -- numactl --physcpubind=0 --interleave=all "$@"
  code=$?
  if [[ "$code" == 0 ]]; then
    python3 "$SOURCE/scripts/summarize_cloudlab_profiles.py" --validate "$folder/$name.json" --trials "$expected_trials" --leaf-count "$LEAVES"
    code=$?
  fi
  if [[ "$code" == 0 ]]; then
    printf '%s\t%s\tcomplete\t0\t%s\n' "$scale" "$name" "$(date -u +%FT%TZ)" >> "$STATUS_FILE"
  else
    printf '%s\t%s\tfailed\t%s\t%s\n' "$scale" "$name" "$code" "$(date -u +%FT%TZ)" >> "$STATUS_FILE"
    FAILURES=$((FAILURES+1))
  fi
}
for SCALE in 1gb 8gb; do
  if [[ "$SCALE" == 1gb ]]; then HEIGHT=24; else HEIGHT=27; fi
  LEAVES=$((1 << HEIGHT))
  FOLDER="$RUN_ROOT/outputs/profiling/$SCALE"
  mkdir -p "$FOLDER"
  # The generic planner emits the shared leaf schedule without a database allocation.
  SUPERRECORD_PLAN_ONLY=1 SUPERRECORD_SKIP_LEGACY=1 \
    SUPERRECORD_MEMORY_BYTES=343597383680 SUPERRECORD_OUTPUT_JSON="$FOLDER/schedule.json" \
    "$BIN" --test tree_superrecord_bench --leaf-count "$LEAVES" --experiments "$TRIALS" --warmup "$WARMUPS" --trial-seed "$TRIAL_SEED" > "$FOLDER/schedule.log" 2>&1
  if [[ "$?" != 0 ]]; then FAILURES=$((FAILURES+1)); continue; fi
  python3 - "$FOLDER/schedule.json" "$FOLDER/leaves.txt" <<'PY'
import json,sys
from pathlib import Path
schedule=json.loads(Path(sys.argv[1]).read_text())['leaf_schedule']
Path(sys.argv[2]).write_text(''.join(str(x)+'\n' for x in schedule['warmup']+schedule['measured']))
PY
  COMMON=(--leaf-count "$LEAVES" --experiments "$TRIALS" --warmup "$WARMUPS" --trial-seed "$TRIAL_SEED")
  run_case "$SCALE" standard "$BIN" --test merkle_benchmarks --benchmark-case standard_onionpir "${COMMON[@]}" --benchmark-json "$FOLDER/standard.json"
  run_case "$SCALE" layerwise "$BIN" --test merkle_benchmarks --benchmark-case merkle_layerwise "${COMMON[@]}" --benchmark-json "$FOLDER/layerwise.json"
  BENCHMARK_OUTPUT_JSON="$FOLDER/tree_g32.json" run_case "$SCALE" tree_g32 "$BIN" --test tree_bench_g32 "${COMMON[@]}"
  BENCHMARK_OUTPUT_JSON="$FOLDER/tree_g1.json" run_case "$SCALE" tree_g1 "$BIN" --test tree_bench "${COMMON[@]}"
  BENCHMARK_OUTPUT_JSON="$FOLDER/tree_compressed.json" run_case "$SCALE" tree_compressed "$BIN" --test tree_compress_bench "${COMMON[@]}"
  BENCHMARK_OUTPUT_JSON="$FOLDER/cuckoo.json" run_case "$SCALE" cuckoo "$BIN" --test cuckoo_bench "${COMMON[@]}"
  SUPERRECORD_SKIP_LEGACY=1 SUPERRECORD_MEMORY_BYTES=343597383680 \
    SUPERRECORD_OUTPUT_JSON="$FOLDER/generic.json" SUPERRECORD_MAX_FRONTIER=2 \
    run_case "$SCALE" generic "$BIN" --test tree_superrecord_bench "${COMMON[@]}"
  TREEPIR_DATA_DIR="$RUN_ROOT/deps/TreePIR/VBPIR_TreePIR" \
    run_case "$SCALE" treepir "$TREEPIR" --height "$HEIGHT" --leaf-file "$FOLDER/leaves.txt" --warmup "$WARMUPS" --trials "$TRIALS" --output "$FOLDER/treepir.json"
  CASE_TRIALS="$FLAT_TRIALS" run_case "$SCALE" flat "$BIN" --test merkle_benchmarks --benchmark-case merkle_paths \
    --leaf-count "$LEAVES" --experiments "$FLAT_TRIALS" --warmup "$WARMUPS" --trial-seed "$TRIAL_SEED" --benchmark-json "$FOLDER/flat.json"
done
python3 "$SOURCE/scripts/summarize_cloudlab_profiles.py" --directory "$RUN_ROOT/outputs/profiling" --trials "$TRIALS" --flat-trials "$FLAT_TRIALS" || FAILURES=$((FAILURES+1))
printf '%s\n' "$FAILURES" > "$RUN_ROOT/metadata/matrix-failures.txt"
if [[ "$FAILURES" == 0 ]]; then echo complete > "$RUN_ROOT/metadata/matrix.status"; else echo failed > "$RUN_ROOT/metadata/matrix.status"; fi
exit "$FAILURES"
