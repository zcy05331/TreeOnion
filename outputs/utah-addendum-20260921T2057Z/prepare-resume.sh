#!/usr/bin/env bash
# Resume prepare-addendum.sh from the TreePIR step; builds and correctness gates before it already passed.
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
status building_treepir_h25
[ -d deps/TreePIR ] || cp -r "$old_root/deps/TreePIR" deps/TreePIR
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
