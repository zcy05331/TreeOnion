#!/usr/bin/env bash
set -euo pipefail
experiment_root=/users/zcy05331/tree-h1-final-20260920T2105Z
cd "$experiment_root"
mkdir -p metadata deps
status() { printf '%s\n' "$1" > metadata/status; date -u +%FT%TZ > metadata/updated-utc.txt; }
trap 'code=$?; if [ "$code" != 0 ]; then status setup_failed; printf "%s\n" "$code" > metadata/setup-exit-code; fi' EXIT
status waiting_idle
python3 wait_idle.py --root "$experiment_root"
status installing_build_tools
sudo -n apt-get update > metadata/apt-update.log 2>&1
sudo -n apt-get install -y --no-install-recommends cmake numactl build-essential pkg-config git curl libssl-dev libboost-dev nlohmann-json3-dev > metadata/apt-install.log 2>&1
lscpu > metadata/lscpu.txt
numactl --hardware > metadata/numa.txt
free -h > metadata/memory-before.txt
g++ --version > metadata/compiler.txt
status waiting_idle
python3 wait_idle.py --root "$experiment_root"
status building_hexl
mkdir deps/hexl-src
git -C deps/hexl-src init > metadata/hexl-fetch.log 2>&1
git -C deps/hexl-src remote add origin https://github.com/intel/hexl.git
git -C deps/hexl-src fetch --depth=1 origin 75a60b8cc908fb4c166315dd8b55b2e1ee7faea7 >> metadata/hexl-fetch.log 2>&1
git -C deps/hexl-src checkout --detach FETCH_HEAD >> metadata/hexl-fetch.log 2>&1
cmake -S deps/hexl-src -B deps/hexl-build -DCMAKE_BUILD_TYPE=Release \
  -DCMAKE_INSTALL_PREFIX="$experiment_root/deps/hexl-install" -DCMAKE_INSTALL_LIBDIR=lib64 \
  -DHEXL_BENCHMARK=OFF -DHEXL_TESTING=OFF > metadata/hexl-configure.log 2>&1
cmake --build deps/hexl-build -j8 > metadata/hexl-build.log 2>&1
cmake --install deps/hexl-build > metadata/hexl-install.log 2>&1
status dependencies_ready
