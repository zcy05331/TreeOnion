#!/usr/bin/env bash
set -uo pipefail
experiment_root=/users/zcy05331/tree-h1-addendum-20260921T2057Z
cd "$experiment_root"
export OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1 MKL_NUM_THREADS=1 PIR_THREADS=1
export CARGO_HOME="$experiment_root/deps/cargo-home" RUSTUP_HOME="$experiment_root/deps/rustup-home"
export PATH="$CARGO_HOME/bin:$PATH" RUSTC_BOOTSTRAP=1
export ONIONPIR_BENCH_COMMIT="$(cat metadata/source-commit.txt)"
export ONIONPIR_BENCH_BRANCH=codex/addendum-2gib-scan-scratch
export ONIONPIR_BENCH_CPU="$(lscpu | sed -n 's/^Model name:[[:space:]]*//p')"
export ONIONPIR_BENCH_OS="$(uname -sr)" ONIONPIR_BENCH_PROCESS_ARCH=x86_64
export ONIONPIR_BENCH_COMPILER="$(g++ --version | head -n1)"
export ONIONPIR_BENCH_HEXL_PATH="$experiment_root/deps/hexl-install/lib64/cmake/hexl-1.2.6"
failures=0
phase() {
  local label="$1"; shift
  printf '%s\n' "$label" > metadata/status
  date -u +%FT%TZ > metadata/updated-utc.txt
  "$@" > "metadata/$label.log" 2>&1
  local code=$?
  printf '%s\t%s\t%s\n' "$(date -u +%FT%TZ)" "$label" "$code" >> metadata/phase-exits.tsv
  if [ "$code" != 0 ]; then failures=$((failures+1)); fi
  return "$code"
}
phase ab_scratch bash ab-scratch.sh
# Every primary row is re-measured with the rebuilt ep3 binary so the five sizes share one binary.
phase primary_matrix python3 source/scripts/run_primary_matrix.py --root "$experiment_root"
phase core_matrix_2gb python3 source/scripts/run_core_matrix.py --root "$experiment_root" --scales 2gb
# Standard re-measured below 2^24 leaves with the 1 GiB first-dimension geometry (expansion height 10, N0 = 512).
phase core_matrix_standard_h10 python3 source/scripts/run_core_matrix.py --root "$experiment_root" \
  --scales 256mb 512mb --cases standard --standard-reference-expansion-height 10 \
  --output-dir "$experiment_root/outputs/core-matrix-standard-h10"
phase treepir_matrix python3 source/scripts/run_treepir_matrix.py --root "$experiment_root" --heights 25
phase external_matrix python3 source/scripts/run_external_matrix.py --root "$experiment_root" \
  --source-commit "$(cat metadata/source-commit.txt)" --hexl-root "$experiment_root/deps/hexl-install" --skip-build --heights 25
if [ "$failures" != 0 ]; then printf 'complete_with_failures\n' > metadata/status; else printf 'complete\n' > metadata/status; fi
date -u +%FT%TZ > metadata/updated-utc.txt
