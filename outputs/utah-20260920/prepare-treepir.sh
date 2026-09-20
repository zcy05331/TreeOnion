#!/usr/bin/env bash
set -euo pipefail
experiment_root=/users/zcy05331/tree-h1-final-20260920T2105Z
cd "$experiment_root"
idle() { python3 "$experiment_root/wait_idle.py" --root "$experiment_root"; }
git -C deps/TreePIR apply "$experiment_root/source/scripts/benchmarks/treepir-scale-prototype.patch"
idle
cmake -S deps/TreePIR/VBPIR_TreePIR -B deps/TreePIR/VBPIR_TreePIR/build \
  -DCMAKE_BUILD_TYPE=Release '-DCMAKE_CXX_FLAGS=-O3 -march=native -mtune=native' \
  -DSEAL_DIR="$experiment_root/deps/seal-install/lib/cmake/SEAL-4.0" \
  > metadata/treepir-configure.log 2>&1
cmake --build deps/TreePIR/VBPIR_TreePIR/build -j8 > metadata/treepir-build.log 2>&1
idle
cmake -S source/scripts/treepir_sealpir -B build-treepir-sealpir \
  -DCMAKE_BUILD_TYPE=Release '-DCMAKE_CXX_FLAGS=-O3 -march=native -mtune=native' \
  -DSEAL_DIR="$experiment_root/deps/seal-install/lib/cmake/SEAL-4.0" \
  -DSEALPIRPLUS_DIR="$experiment_root/deps/TreePIR/SealPIRplus" \
  > metadata/sealpir-configure.log 2>&1
cmake --build build-treepir-sealpir -j8 > metadata/sealpir-build.log 2>&1
idle
TREEPIR_SEALPIR_ADAPTER="$experiment_root/build-treepir-sealpir/treepir_sealpir_adapter" \
  python3 source/scripts/treepir_sealpir/tests/test_treepir_sealpir_adapter.py > metadata/sealpir-tests.log 2>&1
# Validate the newly admitted public heights before any large encrypted setup.
printf '0\n1\n' > metadata/treepir-gate-leaves.txt
for height in 22 23 24 26; do
  "$experiment_root/deps/TreePIR/VBPIR_TreePIR/build/bin/vbpir_treepir" \
    --height "$height" --leaf-file "$experiment_root/metadata/treepir-gate-leaves.txt" \
    --warmup 1 --trials 1 --validate-index-only --output "$experiment_root/metadata/treepir-index-h$height.json" \
    > "metadata/treepir-index-h$height.log" 2>&1
done
