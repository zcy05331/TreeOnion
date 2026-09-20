#!/usr/bin/env bash
set -euo pipefail
experiment_root=/users/zcy05331/tree-h1-final-20260920T2105Z
cd "$experiment_root"
mkdir -p source outputs metadata
export OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1 MKL_NUM_THREADS=1 PIR_THREADS=1
export CARGO_HOME="$experiment_root/deps/cargo-home" RUSTUP_HOME="$experiment_root/deps/rustup-home"
export PATH="$CARGO_HOME/bin:$PATH"
export RUSTC_BOOTSTRAP=1 RESPIRE_SOURCE_DIR="$experiment_root/deps/respire"
status() { printf '%s\n' "$1" > metadata/status; date -u +%FT%TZ > metadata/updated-utc.txt; }
trap 'code=$?; if [ "$code" != 0 ]; then status build_or_gate_failed; printf "%s\n" "$code" > metadata/build-exit-code; fi' EXIT
idle() { python3 "$experiment_root/wait_idle.py" --root "$experiment_root"; }
tar -xzf source.tar.gz -C source
cp source/scripts/wait_for_idle.py wait_idle.py
for variant in default ep3; do
 idle
 status building_$variant
 opts=()
 if [ "$variant" = ep3 ]; then opts=(-DPIR_EP_ROWS=3 -DPIR_EP_LOW_BITS=32); fi
 cmake -S source -B "build-$variant" -DCMAKE_BUILD_TYPE=Benchmark \
  -DACTIVE_CONFIG=CONFIG_N2048_K1_COMP \
  -DHEXL_DIR="$experiment_root/deps/hexl-install/lib64/cmake/hexl-1.2.6" "${opts[@]}" \
  > "metadata/configure-$variant.log" 2>&1
 cmake --build "build-$variant" -j8 > "metadata/build-$variant.log" 2>&1
 idle
 status correctness_$variant
 if [ "$variant" = default ]; then
  numactl --physcpubind=0 --preferred=0 "$experiment_root/build-default/Onion-PIR" \
   --test all --experiments 3 --warmup 1 > metadata/correctness-default.log 2>&1
 else
  numactl --physcpubind=0 --preferred=0 "$experiment_root/build-ep3/Onion-PIR" --test tree_multiplane > metadata/correctness-ep3.log 2>&1
  numactl --physcpubind=0 --preferred=0 "$experiment_root/build-ep3/Onion-PIR" --test tree_noise_envelope > metadata/noise-ep3.log 2>&1
 fi
done
idle
status building_spirit_rc
cmake -S source/scripts/path_baselines/spirit -B build-spirit-external -DCMAKE_BUILD_TYPE=Release \
 '-DCMAKE_CXX_FLAGS=-O3 -march=native -mtune=native' -DSEAL_USE_INTEL_HEXL=ON -DSEAL_USE_MSGSL=OFF \
 -DSPIRIT_DIR="$experiment_root/deps/SPIRIT" \
 -DSPIRIT_SOURCE_COMMIT=7e4c114807d5a83ae5cbad17c514df1dbc32dfc3 > metadata/spirit-configure.log 2>&1
cmake --build build-spirit-external -j8 > metadata/spirit-build.log 2>&1
idle
status testing_spirit_rc
ctest --test-dir build-spirit-external --output-on-failure > metadata/spirit-tests.log 2>&1
idle
status testing_spirit_wrapper
PYTHONPATH="$experiment_root/source" SPIRIT_SOURCE_DIR="$experiment_root/deps/SPIRIT" SPIRIT_PATH_ADAPTER="$experiment_root/build-spirit-external/spirit_path_adapter" \
 python3 -m unittest discover -s source/scripts/tests -p test_spirit_path_adapter.py -v > metadata/spirit-wrapper-tests.log 2>&1
status ready_for_matrix
