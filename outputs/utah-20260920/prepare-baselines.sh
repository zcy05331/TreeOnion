#!/usr/bin/env bash
set -euo pipefail
experiment_root=/users/zcy05331/tree-h1-final-20260920T2105Z
cd "$experiment_root"
status() { printf '%s\n' "$1" > metadata/status; date -u +%FT%TZ > metadata/updated-utc.txt; }
trap 'code=$?; if [ "$code" != 0 ]; then status baseline_dependencies_failed; printf "%s\n" "$code" > metadata/baseline-setup-exit-code; fi' EXIT
idle() { python3 "$experiment_root/wait_idle.py" --root "$experiment_root"; }
clone_pin() {
 local name="$1" url="$2" revision="$3"
 idle
 git init "deps/$name"
 git -C "deps/$name" remote add origin "$url"
 git -C "deps/$name" fetch --depth=1 origin "$revision"
 git -C "deps/$name" checkout --detach FETCH_HEAD
 git -C "deps/$name" rev-parse HEAD > "metadata/$name-commit.txt"
}
status preparing_baseline_sources
clone_pin TreePIR https://github.com/PIR-PIXR/TreePIR.git 930063c5aefc441244abb4890fdf35383f3aa956
clone_pin SPIRIT https://github.com/zhouzhangwalker/SPIRIT.git 7e4c114807d5a83ae5cbad17c514df1dbc32dfc3
clone_pin respire https://github.com/AMACB/respire.git fd20d38e9e94c0298ab21af2f7699a23729eef67
clone_pin SEAL-4.0.0 https://github.com/cnquang/SEAL-4.0.0.git 7e21ef9353ef8e6c5e1f646c9ed8ff486d2ddb8c
idle
status building_seal
cmake -S deps/SEAL-4.0.0 -B deps/seal-build -DCMAKE_BUILD_TYPE=Release \
 '-DCMAKE_CXX_FLAGS=-O3 -march=native -mtune=native' \
 -DCMAKE_INSTALL_PREFIX="$experiment_root/deps/seal-install" \
 -DSEAL_USE_MSGSL=OFF -DSEAL_USE_ZLIB=OFF -DSEAL_USE_ZSTD=OFF \
 -DSEAL_USE_INTEL_HEXL=ON -DHEXL_DIR="$experiment_root/deps/hexl-install/lib64/cmake/hexl-1.2.6" \
 -DSEAL_BUILD_EXAMPLES=OFF -DSEAL_BUILD_TESTS=OFF -DSEAL_BUILD_BENCH=OFF \
 > metadata/seal-configure.log 2>&1
cmake --build deps/seal-build -j8 > metadata/seal-build.log 2>&1
cmake --install deps/seal-build > metadata/seal-install.log 2>&1
idle
status installing_rust
export CARGO_HOME="$experiment_root/deps/cargo-home" RUSTUP_HOME="$experiment_root/deps/rustup-home"
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs -o deps/rustup-init.sh
sh deps/rustup-init.sh -y --no-modify-path --profile minimal --default-toolchain stable > metadata/rust-install.log 2>&1
"$CARGO_HOME/bin/rustc" --version > metadata/rustc.txt
status baseline_dependencies_ready
