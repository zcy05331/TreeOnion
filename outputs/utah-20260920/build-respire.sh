#!/usr/bin/env bash
set -euo pipefail
experiment_root=/users/zcy05331/tree-h1-final-20260920T2105Z
cd "$experiment_root"
export CARGO_HOME="$experiment_root/deps/cargo-home" RUSTUP_HOME="$experiment_root/deps/rustup-home"
export PATH="$CARGO_HOME/bin:$PATH" RUSTC_BOOTSTRAP=1 CARGO_BUILD_JOBS=2
export RESPIRE_SOURCE_DIR="$experiment_root/deps/respire"
export CARGO_TARGET_DIR="$experiment_root/source/.benchmarks/respire-path/cargo-target-external"
export RUSTFLAGS='-C target-cpu=native'
python3 wait_idle.py --root "$experiment_root"
printf 'building_respire\n' > metadata/status
bash source/scripts/path_baselines/respire/stage_respire.sh > metadata/respire-stage.log 2>&1
cargo build --locked --release --manifest-path "$experiment_root/source/.benchmarks/respire-path/respire/Cargo.toml" --bin path_workload > metadata/respire-build.log 2>&1
python3 wait_idle.py --root "$experiment_root"
printf 'testing_respire\n' > metadata/status
cd source
RESPIRE_PATH_ADAPTER_BIN="$CARGO_TARGET_DIR/release/path_workload" python3 -m unittest scripts.tests.test_respire_path_adapter -v > ../metadata/respire-tests.log 2>&1
printf 'ready_for_matrix\n' > ../metadata/status
