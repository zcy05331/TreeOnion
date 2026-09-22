#!/usr/bin/env bash
# One-shot retained-surface regression run.
#
#   scripts/run_regression.sh [path/to/Onion-PIR ...]
#
# The script covers the current public surface: the in-process C++ regression
# suite, retained Merkle/Layerwise/Cuckoo/Flat benchmark JSON writers and their
# Python consumers, the h=1 old-index primary benchmark driver, and the script
# unit tests. Retired super-record h3, standalone Tree g32/g1/compress, t257,
# VBPIR+PBC, InsPIRe and TreePIR+Spiral paths are intentionally absent.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SCRIPTS="$ROOT/scripts"

BINS=("$@")
[[ ${#BINS[@]} -eq 0 ]] && BINS=(build-x86_64-benchmark/Onion-PIR)
for BIN in "${BINS[@]}"; do
  if [[ ! -x "$BIN" ]]; then
    echo "error: binary not found or not executable: $BIN" >&2
    exit 2
  fi
done

WORK="$(mktemp -d "${TMPDIR:-/tmp}/onionpir-regression.XXXXXX")"
finish() {
  local code=$?
  if [[ $code -ne 0 && -d "$WORK" ]]; then
    local log
    log="$(find "$WORK" -type f -name '*.log' -exec ls -t {} + 2>/dev/null | head -n 1)"
    printf '\nregression FAILED (exit %s)\n' "$code" >&2
    if [[ -n "$log" ]]; then
      printf 'last log written (%s), final 30 lines:\n\n' "$log" >&2
      tail -n 30 "$log" >&2
    fi
  fi
  rm -rf "$WORK"
}
trap finish EXIT

step() { printf '\n==> %s\n' "$*"; }

run_in_process_suite() {
  local BIN="$1" TAG="$2" W="$3"
  step "[$TAG] in-process regression suite (--test all)"
  "$BIN" --test all --experiments 3 --warmup 1 > "$W/all.log" 2>&1
  grep -q "Regression suite: .* tests passed" "$W/all.log"
  sed -n '/^Regression suite:/,$p' "$W/all.log"
}

assert_pir_iterations() {
  local TAG="$1" W="$2"
  step "[$TAG] --test pir honors experiments and warmup"
  awk '/^Running test: /{keep = ($0 == "Running test: pir")} keep' "$W/all.log" > "$W/pir.log"
  local ran
  ran="$(grep -c '^======================== Experiment ' "$W/pir.log" || true)"
  [[ "$ran" == 4 ]] || { echo "FAILED: pir ran $ran iterations, expected 4" >&2; exit 1; }
  grep -q "Success rate: 4/4" "$W/pir.log"
  grep -q "Online Average Results (after first 1 warm-up runs)" "$W/pir.log"
  if grep -q "Not enough experiments" "$W/pir.log"; then
    echo "FAILED: pir averages had fewer records than the warmup count" >&2
    exit 1
  fi
  grep -Eq '^Server throughput: [0-9]+(\.[0-9]+)? MB/s' "$W/pir.log"
}

run_retained_cli_contracts() {
  local BIN="$1" TAG="$2" W="$3"
  local PROFILE="$W/layer-profile.json"

  step "[$TAG] layer layout profile and profiled Layerwise benchmark"
  "$BIN" --test layer_layout_sweep --leaf-count 256 --warmup 1 --experiments 2 \
    --trial-seed 7 --layer-padding-budget 1.25 \
    --layout-profile-json "$PROFILE" > "$W/sweep.log" 2>&1
  grep -q '"schema_version": "layer-layout-profile-v1"' "$PROFILE"
  grep -q '"tree_height": 8' "$PROFILE"

  "$BIN" --test merkle_benchmarks --benchmark-case merkle_layerwise \
    --leaf-count 256 --warmup 0 --experiments 1 --trial-seed 7 \
    --layer-layout-policy profiled --layer-layout-profile "$PROFILE" \
    --benchmark-json "$W/profiled.json" > "$W/profiled.log" 2>&1
  grep -q '"layer_layout_policy": "profiled"' "$W/profiled.json"
  grep -q '"correctness_passed": true' "$W/profiled.json"

  step "[$TAG] standard/flat Merkle JSON and Cuckoo tuned pilot JSON validate"
  "$BIN" --test merkle_benchmarks --benchmark-case all \
    --leaf-count 256 --warmup 0 --experiments 1 --trial-seed 7 \
    --benchmark-json "$W/merkle-all.json" > "$W/merkle-all.log" 2>&1
  python3 "$SCRIPTS/summarize_cloudlab_profiles.py" --validate "$W/merkle-all.json" \
    --trials 1 --leaf-count 256 | grep -q '^valid merkle'

  for h in 1 2 3; do
    CUCKOO_LAYOUT_HEIGHT=$h BENCHMARK_OUTPUT_JSON="$W/cuckoo-h$h.json" \
      "$BIN" --test cuckoo_bench --leaf-count 256 --warmup 0 --experiments 1 \
      --trial-seed 7 > "$W/cuckoo-h$h.log" 2>&1
    python3 "$SCRIPTS/summarize_cloudlab_profiles.py" --validate "$W/cuckoo-h$h.json" \
      --trials 1 --leaf-count 256 | grep -q '^valid sampled'
    printf '{"exit_code": 0}\n' > "$W/cuckoo-h$h-process.json"
  done
  python3 "$SCRIPTS/select_cuckoo_layout.py" --output "$W/cuckoo-selection.json" \
    --expected-trials 1 --expected-warmups 0 --leaf-count 256 --heights 1,2,3 \
    "$W/cuckoo-h1.json" "$W/cuckoo-h2.json" "$W/cuckoo-h3.json" > "$W/cuckoo-select.log"
}

run_primary_smoke() {
  local BIN="$1" TAG="$2" W="$3"
  step "[$TAG] primary h1 old-index driver smoke"
  python3 "$SCRIPTS/benchmark_tree_multiplane.py" --binary "$BIN" \
    --output-dir "$W/primary" --leaf-count 1024 --entry-bits 256 \
    --n0-values 4 --experiments 1 --warmup 0 --trial-seed 7 \
    --packing mlwe > "$W/primary.log" 2>&1
  test -s "$W/primary/ranking.json"
}

run_python_tests() {
  local BIN="$1"
  local ABS_BIN
  ABS_BIN="$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")"
  step "script unit tests for retained runners"
  local modules=(
    scripts.tests.test_core_matrix
    scripts.tests.test_external_matrix
    scripts.tests.test_primary_matrix
    scripts.tests.test_path_baseline_report
    scripts.tests.test_path_baseline_runner
    scripts.tests.test_path_baseline_workload
    scripts.tests.test_profile_policy_separation
    scripts.tests.test_read_process_peak_memory
    scripts.tests.test_respire_path_adapter
    scripts.tests.test_run_py
    scripts.tests.test_select_cuckoo_layout
    scripts.tests.test_summarize_cloudlab_profiles
    scripts.tests.test_summarize_layer_layout_profile
    scripts.tests.test_tree_multiplane_benchmark
    scripts.tests.test_treepir_backend_tuning
    scripts.tests.test_treepir_matrix
    scripts.tests.test_spiral_pbc_matrix
    scripts.tests.test_wait_for_idle
  )
  ( cd "$ROOT" && ONIONPIR_BENCH_BIN="$ABS_BIN" \
      python3 -m unittest "${modules[@]}" -v )
}

for BIN in "${BINS[@]}"; do
  ABS_BIN="$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")"
  TAG="$(basename "$(dirname "$ABS_BIN")")/$(basename "$ABS_BIN")"
  BW="$(mktemp -d "$WORK/bin.XXXXXX")"
  run_in_process_suite "$ABS_BIN" "$TAG" "$BW"
  assert_pir_iterations "$TAG" "$BW"
  run_retained_cli_contracts "$ABS_BIN" "$TAG" "$BW"
  run_primary_smoke "$ABS_BIN" "$TAG" "$BW"
done

run_python_tests "${BINS[0]}"

printf '\nAll retained regression checks passed.\n'
