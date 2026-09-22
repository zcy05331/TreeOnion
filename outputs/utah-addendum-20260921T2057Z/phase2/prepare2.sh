#!/usr/bin/env bash
set -euo pipefail
experiment_root=/users/zcy05331/tree-h1-addendum2-20260922T0143Z
old_root=/users/zcy05331/tree-h1-final-20260920T2105Z
cd "$experiment_root"
mkdir -p source outputs metadata deps
export OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1 MKL_NUM_THREADS=1 PIR_THREADS=1
status() { printf '%s\n' "$1" > metadata/status; date -u +%FT%TZ > metadata/updated-utc.txt; }
trap 'code=$?; if [ "$code" != 0 ]; then status prepare_failed; printf "%s\n" "$code" > metadata/prepare-exit-code; fi' EXIT
idle() { python3 "$experiment_root/wait_idle.py" --root "$experiment_root"; }
status extracting_source
tar -xzf source.tar.gz -C source
cp source/scripts/wait_for_idle.py wait_idle.py
ln -sfn "$old_root/deps/hexl-install" deps/hexl-install
lscpu > metadata/lscpu.txt; free -h > metadata/memory-before.txt; g++ --version > metadata/compiler.txt
idle
status building_default
cmake -S source -B build-default -DCMAKE_BUILD_TYPE=Benchmark -DACTIVE_CONFIG=CONFIG_N2048_K1_COMP \
  -DHEXL_DIR="$experiment_root/deps/hexl-install/lib64/cmake/hexl-1.2.6" > metadata/configure-default.log 2>&1
cmake --build build-default -j8 > metadata/build-default.log 2>&1
idle
status correctness_default
numactl --physcpubind=0 --preferred=0 build-default/Onion-PIR --test all --experiments 3 --warmup 1 > metadata/correctness-default.log 2>&1
status ready_for_phase2
