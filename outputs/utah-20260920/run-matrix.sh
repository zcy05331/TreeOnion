#!/usr/bin/env bash
set -uo pipefail
experiment_root=/users/zcy05331/tree-h1-final-20260920T2105Z
cd "$experiment_root"
export OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1 MKL_NUM_THREADS=1 PIR_THREADS=1
export CARGO_HOME="$experiment_root/deps/cargo-home" RUSTUP_HOME="$experiment_root/deps/rustup-home"
export PATH="$CARGO_HOME/bin:$PATH" RUSTC_BOOTSTRAP=1
export ONIONPIR_BENCH_COMMIT="$(cat metadata/source-commit.txt)"
export ONIONPIR_BENCH_BRANCH=main
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
mkdir -p outputs/speed-reference
python3 wait_idle.py --root "$experiment_root"
phase standard_1gib_reference python3 source/scripts/read_process_peak_memory.py \
  --log outputs/speed-reference/standard.log --output outputs/speed-reference/standard-process.json -- \
  numactl --physcpubind=0 --preferred=0 "$experiment_root/build-default/Onion-PIR" \
  --test merkle_benchmarks --benchmark-case standard_onionpir --leaf-count 16777216 \
  --experiments 50 --warmup 3 --trial-seed 7 --benchmark-json "$experiment_root/outputs/speed-reference/standard.json"
phase validate_standard_1gib python3 source/scripts/summarize_cloudlab_profiles.py \
  --validate outputs/speed-reference/standard.json --trials 50 --leaf-count 16777216
phase primary_matrix python3 "$experiment_root/source/scripts/run_primary_matrix.py" --root "$experiment_root"
phase core_matrix python3 "$experiment_root/source/scripts/run_core_matrix.py" --root "$experiment_root" \
  --standard-1gb-source "$experiment_root/outputs/speed-reference"
if phase treepir_prepare bash "$experiment_root/prepare-treepir.sh"; then
  phase treepir_matrix python3 "$experiment_root/source/scripts/run_treepir_matrix.py" --root "$experiment_root"
fi
phase external_matrix python3 "$experiment_root/source/scripts/run_external_matrix.py" \
  --root "$experiment_root" --source-commit "$(cat metadata/source-commit.txt)" \
  --hexl-root "$experiment_root/deps/hexl-install" --skip-build
if [ "$failures" != 0 ]; then
  printf 'complete_with_failures\n' > metadata/status
else
  python3 - "$experiment_root" <<'PY'
import json,sys
from pathlib import Path
root=Path(sys.argv[1])
external=json.loads((root/'metadata/external-matrix/progress.json').read_text())
state='complete_with_resource_skips' if external['state']=='complete_with_resource_skips' else 'complete'
(root/'metadata/status').write_text(state+'\n')
PY
fi
date -u +%FT%TZ > metadata/updated-utc.txt
