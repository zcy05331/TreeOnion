#!/usr/bin/env bash
set -euo pipefail
TASK_ROOT=/users/zcy05331/generic-superrecord-bench-20260904/revision-73a9253
trap 'echo failed > "$TASK_ROOT/metadata/native-build.status"' ERR
echo building > "$TASK_ROOT/metadata/native-build.status"
cd "$TASK_ROOT/source"
export OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1 MKL_NUM_THREADS=1
export ONIONPIR_BENCH_COMMIT=73a925300f6d45183063b41cc372a6e8c349e4a8
export SUPERRECORD_COMMIT="$ONIONPIR_BENCH_COMMIT"
cmake -S . -B build-native -DCMAKE_BUILD_TYPE=Benchmark -DACTIVE_CONFIG=CONFIG_N2048_K1_COMP -DHEXL_DIR="$TASK_ROOT/deps/hexl-install/lib64/cmake/hexl-1.2.6" > "$TASK_ROOT/metadata/onion-native-build.log" 2>&1
cmake --build build-native -j4 >> "$TASK_ROOT/metadata/onion-native-build.log" 2>&1
echo checking > "$TASK_ROOT/metadata/native-build.status"
bash scripts/run_regression.sh ./build-native/Onion-PIR > "$TASK_ROOT/metadata/native-regression.log" 2>&1
BENCHMARK_OUTPUT_JSON="$TASK_ROOT/outputs/native-smoke/cuckoo.json" ./build-native/Onion-PIR --test cuckoo_bench --leaf-count 8192 --experiments 2 --warmup 1 --trial-seed 7 > "$TASK_ROOT/outputs/native-smoke/cuckoo.log" 2>&1
python3 scripts/summarize_cloudlab_profiles.py --validate "$TASK_ROOT/outputs/native-smoke/cuckoo.json" --trials 2 --leaf-count 8192
SUPERRECORD_OUTPUT_JSON="$TASK_ROOT/outputs/native-smoke/generic.json" ./build-native/Onion-PIR --test tree_superrecord_bench --leaf-count 1024 --experiments 2 --warmup 1 --trial-seed 7 > "$TASK_ROOT/outputs/native-smoke/generic.log" 2>&1
python3 scripts/summarize_cloudlab_profiles.py --validate "$TASK_ROOT/outputs/native-smoke/generic.json" --trials 2 --leaf-count 1024
echo complete > "$TASK_ROOT/metadata/native-build.status"
