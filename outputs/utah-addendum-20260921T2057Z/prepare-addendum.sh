#!/usr/bin/env bash
set -euo pipefail
experiment_root=/users/zcy05331/tree-h1-addendum-20260921T2057Z
old_root=/users/zcy05331/tree-h1-final-20260920T2105Z
cd "$experiment_root"
mkdir -p source outputs metadata deps
export OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1 MKL_NUM_THREADS=1 PIR_THREADS=1
export CARGO_HOME="$old_root/deps/cargo-home" RUSTUP_HOME="$old_root/deps/rustup-home"
export PATH="$CARGO_HOME/bin:$PATH" RUSTC_BOOTSTRAP=1
status() { printf '%s\n' "$1" > metadata/status; date -u +%FT%TZ > metadata/updated-utc.txt; }
trap 'code=$?; if [ "$code" != 0 ]; then status prepare_failed; printf "%s\n" "$code" > metadata/prepare-exit-code; fi' EXIT
idle() { python3 "$experiment_root/wait_idle.py" --root "$experiment_root"; }
status extracting_source
tar -xzf source.tar.gz -C source
cp source/scripts/wait_for_idle.py wait_idle.py
# Unchanged dependencies and unchanged adapter builds are shared with the completed campaign root.
for dep in hexl-install seal-install SPIRIT respire cargo-home rustup-home; do
  ln -sfn "$old_root/deps/$dep" "deps/$dep"
done
ln -sfn "$old_root/build-spirit-external" build-spirit-external
ln -sfn "$old_root/build-treepir-sealpir" build-treepir-sealpir
lscpu > metadata/lscpu.txt; free -h > metadata/memory-before.txt; g++ --version > metadata/compiler.txt
idle
status building_default_ep3
for variant in default ep3; do
  opts=()
  if [ "$variant" = ep3 ]; then opts=(-DPIR_EP_ROWS=3 -DPIR_EP_LOW_BITS=32); fi
  cmake -S source -B "build-$variant" -DCMAKE_BUILD_TYPE=Benchmark -DACTIVE_CONFIG=CONFIG_N2048_K1_COMP \
    -DHEXL_DIR="$experiment_root/deps/hexl-install/lib64/cmake/hexl-1.2.6" "${opts[@]}" > "metadata/configure-$variant.log" 2>&1
  cmake --build "build-$variant" -j8 > "metadata/build-$variant.log" 2>&1
done
idle
status correctness_default
numactl --physcpubind=0 --preferred=0 build-default/Onion-PIR --test all --experiments 3 --warmup 1 > metadata/correctness-default.log 2>&1
idle
status correctness_ep3
numactl --physcpubind=0 --preferred=0 build-ep3/Onion-PIR --test tree_multiplane > metadata/correctness-ep3.log 2>&1
numactl --physcpubind=0 --preferred=0 build-ep3/Onion-PIR --test tree_noise_envelope > metadata/noise-ep3.log 2>&1
idle
status building_treepir_h25
# Private copy of the patched TreePIR checkout: revert the old patch, apply the one that admits height 25, rebuild.
cp -r "$old_root/deps/TreePIR" deps/TreePIR
rm -rf deps/TreePIR/VBPIR_TreePIR/build
git -C deps/TreePIR checkout -- .
git -C deps/TreePIR apply "$experiment_root/source/scripts/benchmarks/treepir-scale-prototype.patch"
cmake -S deps/TreePIR/VBPIR_TreePIR -B deps/TreePIR/VBPIR_TreePIR/build -DCMAKE_BUILD_TYPE=Release \
  '-DCMAKE_CXX_FLAGS=-O3 -march=native -mtune=native' -DSEAL_DIR="$experiment_root/deps/seal-install/lib/cmake/SEAL-4.0" \
  > metadata/treepir-configure.log 2>&1
cmake --build deps/TreePIR/VBPIR_TreePIR/build -j8 > metadata/treepir-build.log 2>&1
printf '0\n1\n' > metadata/treepir-gate-leaves.txt
for height in 24 25; do
  deps/TreePIR/VBPIR_TreePIR/build/bin/vbpir_treepir --height "$height" --leaf-file metadata/treepir-gate-leaves.txt \
    --warmup 1 --trials 1 --validate-index-only --output "metadata/treepir-index-h$height.json" > "metadata/treepir-index-h$height.log" 2>&1
done
idle
status building_respire_h25
export RESPIRE_SOURCE_DIR="$experiment_root/deps/respire"
bash source/scripts/path_baselines/respire/stage_respire.sh > metadata/respire-stage.log 2>&1
CARGO_TARGET_DIR="$experiment_root/source/.benchmarks/respire-path/cargo-target-external" RUSTFLAGS="-C target-cpu=native" \
  cargo build --locked --release --manifest-path source/.benchmarks/respire-path/respire/Cargo.toml --bin path_workload > metadata/respire-build.log 2>&1
status ready_for_addendum
