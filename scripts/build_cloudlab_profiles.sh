#!/usr/bin/env bash
# Native builds and bounded correctness gates before the measured matrix.
set -euo pipefail
RUN_ROOT="${1:?usage: build_cloudlab_profiles.sh RUN_ROOT}"
SOURCE="$RUN_ROOT/source"
META="$RUN_ROOT/metadata"
mkdir -p "$META" "$RUN_ROOT/outputs/native-smoke"
trap 'echo failed > "$META/native-build.status"' ERR
echo building > "$META/native-build.status"
if [[ ! -d "$RUN_ROOT/deps/TreePIR/.git" ]]; then
  git clone https://github.com/PIR-PIXR/TreePIR.git "$RUN_ROOT/deps/TreePIR"
  git -C "$RUN_ROOT/deps/TreePIR" checkout 930063c5aefc441244abb4890fdf35383f3aa956
  git -C "$RUN_ROOT/deps/TreePIR" apply "$SOURCE/scripts/benchmarks/treepir-scale-prototype.patch"
fi
(
  cmake -S "$SOURCE" -B "$SOURCE/build-native" \
    -DCMAKE_BUILD_TYPE=Benchmark -DACTIVE_CONFIG=CONFIG_N2048_K1_COMP \
    -DHEXL_DIR="$RUN_ROOT/deps/hexl-install/lib64/cmake/hexl-1.2.6"
  cmake --build "$SOURCE/build-native" -j16
) > "$META/onion-native-build.log" 2>&1 &
ONION_BUILD_PID=$!
(
  cmake -S "$RUN_ROOT/deps/TreePIR/VBPIR_TreePIR" \
    -B "$RUN_ROOT/deps/TreePIR/VBPIR_TreePIR/build" \
    -DCMAKE_BUILD_TYPE=Release '-DCMAKE_CXX_FLAGS=-O3 -march=native -mtune=native' \
    -DSEAL_DIR="$RUN_ROOT/deps/seal-install/lib/cmake/SEAL-4.0"
  cmake --build "$RUN_ROOT/deps/TreePIR/VBPIR_TreePIR/build" -j8
) > "$META/treepir-native-build.log" 2>&1 &
TREEPIR_BUILD_PID=$!
wait "$ONION_BUILD_PID"
wait "$TREEPIR_BUILD_PID"
echo checking > "$META/native-build.status"
cd "$SOURCE"
export OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1 MKL_NUM_THREADS=1
BIN="$SOURCE/build-native/Onion-PIR"
OUT="$RUN_ROOT/outputs/native-smoke"
bash "$SOURCE/scripts/run_regression.sh" "$BIN" > "$META/native-regression.log" 2>&1
for TEST in tree_bench_g32 tree_bench tree_compress_bench cuckoo_bench; do
  LEAVES=1024
  [[ "$TEST" != tree_bench ]] || LEAVES=262144
  BENCHMARK_OUTPUT_JSON="$OUT/$TEST.json" "$BIN" --test "$TEST" \
    --leaf-count "$LEAVES" --experiments 2 --warmup 1 --trial-seed 7 > "$OUT/$TEST.log" 2>&1
done
"$BIN" --test merkle_benchmarks --benchmark-case all --leaf-count 1024 \
  --experiments 2 --warmup 1 --trial-seed 7 --benchmark-json "$OUT/merkle.json" > "$OUT/merkle.log" 2>&1
SUPERRECORD_OUTPUT_JSON="$OUT/generic.json" "$BIN" --test tree_superrecord_bench \
  --leaf-count 1024 --experiments 2 --warmup 1 --trial-seed 7 > "$OUT/generic.log" 2>&1
TREEPIR_DATA_DIR="$RUN_ROOT/deps/TreePIR/VBPIR_TreePIR" \
  "$RUN_ROOT/deps/TreePIR/VBPIR_TreePIR/build/bin/vbpir_treepir" --height 10 \
  --warmup 1 --trials 2 --output "$OUT/treepir.json" > "$OUT/treepir.log" 2>&1
echo complete > "$META/native-build.status"
