#!/usr/bin/env bash
set -uo pipefail
experiment_root=/users/zcy05331/tree-h1-addendum2-20260922T0143Z
cd "$experiment_root"
export OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1 MKL_NUM_THREADS=1 PIR_THREADS=1
export ONIONPIR_BENCH_COMMIT="$(cat metadata/source-commit.txt)" ONIONPIR_BENCH_BRANCH=codex/addendum-2gib-scan-scratch
export ONIONPIR_BENCH_CPU="$(lscpu | sed -n 's/^Model name:[[:space:]]*//p')" ONIONPIR_BENCH_OS="$(uname -sr)" ONIONPIR_BENCH_PROCESS_ARCH=x86_64
export ONIONPIR_BENCH_COMPILER="$(g++ --version | head -n1)" ONIONPIR_BENCH_HEXL_PATH="$experiment_root/deps/hexl-install/lib64/cmake/hexl-1.2.6"
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
# Small sizes under the 1 GiB reference geometry (expansion height 10): the
# remaining Merkle baselines, so every column of the table shares one envelope.
phase core_matrix_h10_small python3 source/scripts/run_core_matrix.py --root "$experiment_root" \
  --scales 256mb 512mb --cases layerwise layerwise_merged cuckoo flat --reference-expansion-height 10 \
  --output-dir "$experiment_root/outputs/core-matrix-h10"
# Layerwise with the tree scheme's response merging at the sizes whose default
# reference already is h10.
phase core_matrix_merged python3 source/scripts/run_core_matrix.py --root "$experiment_root" \
  --scales 1gb 2gb 4gb --cases layerwise_merged --output-dir "$experiment_root/outputs/core-matrix-merged"
if [ "$failures" != 0 ]; then printf 'phase2_complete_with_failures\n' > metadata/status; else printf 'phase2_complete\n' > metadata/status; fi
date -u +%FT%TZ > metadata/updated-utc.txt
