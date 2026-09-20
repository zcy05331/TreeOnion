#!/usr/bin/env bash
set -euo pipefail
experiment_root=/users/zcy05331/tree-h1-old-layout-20260920T2003Z
cd "$experiment_root"
mkdir -p deps metadata
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
if [ ! -d deps/hexl-install ]; then
  cp -a /users/zcy05331/onion-cuckoo-rs-20260919T234720Z/deps/hexl-install deps/hexl-install
fi
idle
sudo -n apt-get update
sudo -n apt-get install -y --no-install-recommends libboost-dev nlohmann-json3-dev libgmp-dev libmpfr-dev libssl-dev pkg-config
clone_pin TreePIR https://github.com/PIR-PIXR/TreePIR.git 930063c5aefc441244abb4890fdf35383f3aa956
clone_pin SPIRIT https://github.com/zhouzhangwalker/SPIRIT.git 7e4c114807d5a83ae5cbad17c514df1dbc32dfc3
clone_pin poulpy-pir https://github.com/poulpy-fhe/poulpy-pir.git 533081a74301c8ba6ddd5e1dfc0c9daa6e3e75ef
clone_pin respire https://github.com/AMACB/respire.git fd20d38e9e94c0298ab21af2f7699a23729eef67
clone_pin SEAL-4.0.0 https://github.com/cnquang/SEAL-4.0.0.git 7e21ef9353ef8e6c5e1f646c9ed8ff486d2ddb8c
idle
cmake -S deps/SEAL-4.0.0 -B deps/seal-build -DCMAKE_BUILD_TYPE=Release \
  '-DCMAKE_CXX_FLAGS=-O3 -march=native -mtune=native' \
  -DCMAKE_INSTALL_PREFIX="$experiment_root/deps/seal-install" \
  -DSEAL_USE_MSGSL=OFF -DSEAL_USE_ZLIB=OFF -DSEAL_USE_ZSTD=OFF \
  -DSEAL_USE_INTEL_HEXL=ON -DHEXL_DIR="$experiment_root/deps/hexl-install/lib64/cmake/hexl-1.2.6" \
  -DSEAL_BUILD_EXAMPLES=OFF -DSEAL_BUILD_TESTS=OFF -DSEAL_BUILD_BENCH=OFF
cmake --build deps/seal-build -j8
cmake --install deps/seal-build
idle
export CARGO_HOME="$experiment_root/deps/cargo-home" RUSTUP_HOME="$experiment_root/deps/rustup-home"
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs -o deps/rustup-init.sh
sh deps/rustup-init.sh -y --no-modify-path --profile minimal --default-toolchain stable
"$CARGO_HOME/bin/rustup" toolchain install nightly-2026-05-14 --profile minimal
"$CARGO_HOME/bin/rustc" --version > metadata/rustc-stable.txt
"$CARGO_HOME/bin/rustc" +nightly-2026-05-14 --version > metadata/rustc-nightly.txt
