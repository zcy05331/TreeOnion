#!/usr/bin/env bash
# Same-day Standard reference rows (default geometry rule) so the rebuilt primary rows can be
# compared per byte against Standard measured on the same day, same binary tree, same idle host.
set -uo pipefail
experiment_root=/users/zcy05331/tree-h1-addendum-20260921T2057Z
cd "$experiment_root"
export OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1 MKL_NUM_THREADS=1 PIR_THREADS=1
export ONIONPIR_BENCH_COMMIT="$(cat metadata/source-commit.txt)" ONIONPIR_BENCH_BRANCH=codex/addendum-2gib-scan-scratch
export ONIONPIR_BENCH_CPU="$(lscpu | sed -n 's/^Model name:[[:space:]]*//p')" ONIONPIR_BENCH_OS="$(uname -sr)" ONIONPIR_BENCH_PROCESS_ARCH=x86_64
export ONIONPIR_BENCH_COMPILER="$(g++ --version | head -n1)" ONIONPIR_BENCH_HEXL_PATH="$experiment_root/deps/hexl-install/lib64/cmake/hexl-1.2.6"
printf 'post_standard_sameday\n' > metadata/status
python3 source/scripts/run_core_matrix.py --root "$experiment_root" --scales 1gb 4gb --cases standard \
  --output-dir "$experiment_root/outputs/core-matrix-sameday" > metadata/post_standard_sameday.log 2>&1
code=$?
printf '%s\t%s\t%s\n' "$(date -u +%FT%TZ)" post_standard_sameday "$code" >> metadata/phase-exits.tsv
if [ "$code" = 0 ]; then printf 'addendum_complete\n' > metadata/status; else printf 'addendum_complete_with_failures\n' > metadata/status; fi
