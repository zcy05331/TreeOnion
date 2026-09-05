#!/usr/bin/env bash
# Future 1 GiB policy: h3/h1, tuned Layerwise, g32, tuned Cuckoo, TreePIR, Flat.
set -uo pipefail
RUN_ROOT="${1:?usage: run_cloudlab_profile_matrix.sh RUN_ROOT}"
SOURCE="$RUN_ROOT/source"
BIN="$SOURCE/build-native/Onion-PIR"
TREEPIR="$RUN_ROOT/deps/TreePIR/VBPIR_TreePIR/build/bin/vbpir_treepir"
TRIALS="${TRIALS:-100}"
CUCKOO_TRIALS="${CUCKOO_TRIALS:-16}"
TREEPIR_TRIALS="${TREEPIR_TRIALS:-16}"
FLAT_TRIALS="${FLAT_TRIALS:-16}"
TUNING_TRIALS="${TUNING_TRIALS:-16}"
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
echo running > "$RUN_ROOT/metadata/matrix.status"
FAILURES=0
run_case() {
  local scale="$1" name="$2"; shift 2
  local folder="$RUN_ROOT/outputs/profiling/$scale${CASE_SUBDIR:+/$CASE_SUBDIR}" code
  local case_label="${CASE_SUBDIR:+$CASE_SUBDIR/}$name"
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
        printf '%s\t%s\tcomplete\t0\t%s\n' "$scale" "$case_label" "$(date -u +%FT%TZ)" >> "$STATUS_FILE"
        return 0
      fi
      echo "Existing completed result does not match this run's sample policy: $folder/$name.json. Use a fresh run directory." >&2
      FAILURES=$((FAILURES+1))
      return 1
    fi
  fi
  printf '%s %s\n' "$scale" "$case_label" > "$RUN_ROOT/metadata/current-job.txt"
  printf '%s\t%s\trunning\t\t%s\n' "$scale" "$case_label" "$(date -u +%FT%TZ)" >> "$STATUS_FILE"
  python3 "$SOURCE/scripts/read_process_peak_memory.py" \
    --log "$folder/$name.log" --output "$folder/$name-process.json" \
    -- numactl --physcpubind=0 --interleave=all "$@"
  code=$?
  if [[ "$code" == 0 ]]; then
    python3 "$SOURCE/scripts/summarize_cloudlab_profiles.py" --validate "$folder/$name.json" --trials "$expected_trials" --leaf-count "$LEAVES"
    code=$?
  fi
  if [[ "$code" == 0 ]]; then
    printf '%s\t%s\tcomplete\t0\t%s\n' "$scale" "$case_label" "$(date -u +%FT%TZ)" >> "$STATUS_FILE"
  else
    printf '%s\t%s\tfailed\t%s\t%s\n' "$scale" "$case_label" "$code" "$(date -u +%FT%TZ)" >> "$STATUS_FILE"
    FAILURES=$((FAILURES+1))
  fi
}
run_layerwise_profile() {
  local scale="$1" profile="$2"
  local folder
  folder="$(dirname "$profile")"
  mkdir -p "$folder"
  if [[ -f "$profile" && -f "$folder/profile-process.json" ]]; then
    if python3 - "$profile" "$folder/profile-process.json" "$TUNING_TRIALS" "$WARMUPS" "$LEAVES" <<'PY'
import json,sys
from pathlib import Path
profile=json.loads(Path(sys.argv[1]).read_text())
process=json.loads(Path(sys.argv[2]).read_text())
ok = (
    process.get("exit_code") == 0
    and profile.get("measured_trials") == int(sys.argv[3])
    and profile.get("warmups") == int(sys.argv[4])
    and profile.get("tree_height") == (int(sys.argv[5]).bit_length() - 1)
    and isinstance(profile.get("measurements"), list)
    and profile["measurements"]
)
sys.exit(0 if ok else 1)
PY
    then
      printf '%s\t%s\tcomplete\t0\t%s\n' "$scale" "tuning/layerwise/profile" "$(date -u +%FT%TZ)" >> "$STATUS_FILE"
      return 0
    fi
  fi
  printf '%s %s\n' "$scale" "tuning/layerwise/profile" > "$RUN_ROOT/metadata/current-job.txt"
  printf '%s\t%s\trunning\t\t%s\n' "$scale" "tuning/layerwise/profile" "$(date -u +%FT%TZ)" >> "$STATUS_FILE"
  python3 "$SOURCE/scripts/read_process_peak_memory.py" \
    --log "$folder/profile.log" --output "$folder/profile-process.json" \
    -- numactl --physcpubind=0 --interleave=all "$BIN" --test layer_layout_sweep \
    --leaf-count "$LEAVES" --experiments "$TUNING_TRIALS" --warmup "$WARMUPS" \
    --trial-seed "$TRIAL_SEED" --layer-padding-budget 1.1 \
    --layout-profile-json "$profile"
  local code=$?
  if [[ "$code" == 0 ]]; then
    printf '%s\t%s\tcomplete\t0\t%s\n' "$scale" "tuning/layerwise/profile" "$(date -u +%FT%TZ)" >> "$STATUS_FILE"
  else
    printf '%s\t%s\tfailed\t%s\t%s\n' "$scale" "tuning/layerwise/profile" "$code" "$(date -u +%FT%TZ)" >> "$STATUS_FILE"
    FAILURES=$((FAILURES+1))
  fi
  return "$code"
}
for SCALE in 1gb; do
  HEIGHT=24
  LEAVES=$((1 << HEIGHT))
  FOLDER="$RUN_ROOT/outputs/profiling/$SCALE"
  mkdir -p "$FOLDER"
  # TreePIR must use its own exact trial count: the 16-trial schedule is not
  # a prefix of the 100-trial schedule generated with the same seed.
  SUPERRECORD_PLAN_ONLY=1 SUPERRECORD_SKIP_LEGACY=1 SUPERRECORD_BUNDLE_ARITIES=1,3 \
    SUPERRECORD_MAX_ARITY=3 \
    SUPERRECORD_MEMORY_BYTES=343597383680 SUPERRECORD_OUTPUT_JSON="$FOLDER/schedule.json" \
    "$BIN" --test tree_superrecord_bench --leaf-count "$LEAVES" --experiments "$TREEPIR_TRIALS" --warmup "$WARMUPS" --trial-seed "$TRIAL_SEED" > "$FOLDER/schedule.log" 2>&1
  if [[ "$?" != 0 ]]; then FAILURES=$((FAILURES+1)); continue; fi
  python3 - "$FOLDER/schedule.json" "$FOLDER/leaves.txt" <<'PY'
import json,sys
from pathlib import Path
schedule=json.loads(Path(sys.argv[1]).read_text())['leaf_schedule']
Path(sys.argv[2]).write_text(''.join(str(x)+'\n' for x in schedule['warmup']+schedule['measured']))
PY
  COMMON=(--leaf-count "$LEAVES" --experiments "$TRIALS" --warmup "$WARMUPS" --trial-seed "$TRIAL_SEED")
  PILOT_COMMON=(--leaf-count "$LEAVES" --experiments "$TUNING_TRIALS" --warmup "$WARMUPS" --trial-seed "$TRIAL_SEED")
  LAYERWISE_PROFILE="$FOLDER/tuning/layerwise/profile.json"
  if run_layerwise_profile "$SCALE" "$LAYERWISE_PROFILE"; then
    run_case "$SCALE" layerwise_tuned "$BIN" --test merkle_benchmarks --benchmark-case merkle_layerwise \
      "${COMMON[@]}" --layer-layout-policy profiled --layer-layout-profile "$LAYERWISE_PROFILE" \
      --layer-padding-budget 1.1 --benchmark-json "$FOLDER/layerwise_tuned.json"
  fi
  BENCHMARK_OUTPUT_JSON="$FOLDER/tree_g32.json" run_case "$SCALE" tree_g32 "$BIN" --test tree_bench_g32 "${COMMON[@]}"
  CUCKOO_SELECTION="$FOLDER/tuning/cuckoo/selection.json"
  rm -f "$CUCKOO_SELECTION"
  for CUCKOO_HEIGHT in 7 8 9 10; do
    CASE_TRIALS="$TUNING_TRIALS" CASE_SUBDIR="tuning/cuckoo" \
      BENCHMARK_OUTPUT_JSON="$FOLDER/tuning/cuckoo/h$CUCKOO_HEIGHT.json" \
      CUCKOO_LAYOUT_HEIGHT="$CUCKOO_HEIGHT" \
      run_case "$SCALE" "h$CUCKOO_HEIGHT" "$BIN" --test cuckoo_bench "${PILOT_COMMON[@]}"
  done
  if CHOSEN_CUCKOO_HEIGHT="$(
    python3 "$SOURCE/scripts/select_cuckoo_layout.py" \
      --output "$CUCKOO_SELECTION" --expected-trials "$TUNING_TRIALS" \
      --expected-warmups "$WARMUPS" --leaf-count "$LEAVES" \
      "$FOLDER/tuning/cuckoo/h7.json" "$FOLDER/tuning/cuckoo/h8.json" \
      "$FOLDER/tuning/cuckoo/h9.json" "$FOLDER/tuning/cuckoo/h10.json"
  )"; then
    CASE_TRIALS="$CUCKOO_TRIALS" BENCHMARK_OUTPUT_JSON="$FOLDER/cuckoo_tuned.json" CUCKOO_LAYOUT_HEIGHT="$CHOSEN_CUCKOO_HEIGHT" \
      run_case "$SCALE" cuckoo_tuned "$BIN" --test cuckoo_bench \
        --leaf-count "$LEAVES" --experiments "$CUCKOO_TRIALS" --warmup "$WARMUPS" --trial-seed "$TRIAL_SEED"
  else
    printf '%s\t%s\tfailed\t%s\t%s\n' "$SCALE" "tuning/cuckoo/selection" "1" "$(date -u +%FT%TZ)" >> "$STATUS_FILE"
    FAILURES=$((FAILURES+1))
  fi
  SUPERRECORD_SKIP_LEGACY=1 SUPERRECORD_BUNDLE_ARITIES=1,3 SUPERRECORD_MAX_ARITY=3 SUPERRECORD_MEMORY_BYTES=343597383680 \
    SUPERRECORD_OUTPUT_JSON="$FOLDER/generic.json" SUPERRECORD_MAX_FRONTIER=2 \
    run_case "$SCALE" generic "$BIN" --test tree_superrecord_bench "${COMMON[@]}"
  CASE_TRIALS="$TREEPIR_TRIALS" TREEPIR_DATA_DIR="$RUN_ROOT/deps/TreePIR/VBPIR_TreePIR" \
    run_case "$SCALE" treepir "$TREEPIR" --height "$HEIGHT" --leaf-file "$FOLDER/leaves.txt" --warmup "$WARMUPS" --trials "$TREEPIR_TRIALS" --output "$FOLDER/treepir.json"
  CASE_TRIALS="$FLAT_TRIALS" run_case "$SCALE" flat "$BIN" --test merkle_benchmarks --benchmark-case merkle_paths \
    --leaf-count "$LEAVES" --experiments "$FLAT_TRIALS" --warmup "$WARMUPS" --trial-seed "$TRIAL_SEED" --benchmark-json "$FOLDER/flat.json"
done
# Each run_case already enforced its requested count; summarize the mixed
# 100/16 sample files using the counts recorded in their own headers.
python3 "$SOURCE/scripts/summarize_cloudlab_profiles.py" --directory "$RUN_ROOT/outputs/profiling" || FAILURES=$((FAILURES+1))
printf '%s\n' "$FAILURES" > "$RUN_ROOT/metadata/matrix-failures.txt"
if [[ "$FAILURES" == 0 ]]; then echo complete > "$RUN_ROOT/metadata/matrix.status"; else echo failed > "$RUN_ROOT/metadata/matrix.status"; fi
exit "$FAILURES"
