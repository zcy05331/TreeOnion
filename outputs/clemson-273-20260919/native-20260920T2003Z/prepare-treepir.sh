#!/usr/bin/env bash
set -euo pipefail
experiment_root=/users/zcy05331/tree-h1-old-layout-20260920T2003Z
cd "$experiment_root"
idle() { python3 "$experiment_root/wait_idle.py" --root "$experiment_root"; }
git -C deps/TreePIR apply "$experiment_root/source/scripts/benchmarks/treepir-scale-prototype.patch"
idle
cmake -S deps/TreePIR/VBPIR_TreePIR -B deps/TreePIR/VBPIR_TreePIR/build \
  -DCMAKE_BUILD_TYPE=Release '-DCMAKE_CXX_FLAGS=-O3 -march=native -mtune=native' \
  -DSEAL_DIR="$experiment_root/deps/seal-install/lib/cmake/SEAL-4.0"
cmake --build deps/TreePIR/VBPIR_TreePIR/build -j8
idle
cmake -S source/scripts/treepir_sealpir -B build-treepir-sealpir \
  -DCMAKE_BUILD_TYPE=Release '-DCMAKE_CXX_FLAGS=-O3 -march=native -mtune=native' \
  -DSEAL_DIR="$experiment_root/deps/seal-install/lib/cmake/SEAL-4.0" \
  -DSEALPIRPLUS_DIR="$experiment_root/deps/TreePIR/SealPIRplus"
cmake --build build-treepir-sealpir -j8
idle
TREEPIR_SEALPIR_ADAPTER="$experiment_root/build-treepir-sealpir/treepir_sealpir_adapter" \
  python3 source/scripts/treepir_sealpir/tests/test_treepir_sealpir_adapter.py
