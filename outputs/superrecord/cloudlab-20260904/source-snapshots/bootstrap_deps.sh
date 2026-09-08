#!/usr/bin/env bash
# Build the two selected PIR dependencies under the experiment directory.
set -euo pipefail
RUN_ROOT="${1:?usage: bootstrap_cloudlab_dependencies.sh RUN_ROOT}"
mkdir -p "$RUN_ROOT/deps" "$RUN_ROOT/metadata"
if [[ ! -d "$RUN_ROOT/deps/hexl-src/.git" ]]; then
  git clone --depth 1 --branch v1.2.6 https://github.com/intel/hexl.git "$RUN_ROOT/deps/hexl-src" ||
  git clone --depth 1 --branch 1.2.6 https://github.com/intel/hexl.git "$RUN_ROOT/deps/hexl-src"
fi
if [[ ! -d "$RUN_ROOT/deps/SEAL-4.0.0/.git" ]]; then
  git clone https://github.com/cnquang/SEAL-4.0.0.git "$RUN_ROOT/deps/SEAL-4.0.0"
fi
git -C "$RUN_ROOT/deps/SEAL-4.0.0" checkout 7e21ef9353ef8e6c5e1f646c9ed8ff486d2ddb8c
(
  cmake -S "$RUN_ROOT/deps/hexl-src" -B "$RUN_ROOT/deps/hexl-build" \
    -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$RUN_ROOT/deps/hexl-install" \
    -DCMAKE_INSTALL_LIBDIR=lib64 -DHEXL_BENCHMARK=OFF -DHEXL_TESTING=OFF
  cmake --build "$RUN_ROOT/deps/hexl-build" -j8
  cmake --install "$RUN_ROOT/deps/hexl-build"
) > "$RUN_ROOT/metadata/hexl-build.log" 2>&1 &
HEXL_PID=$!
(
  cmake -S "$RUN_ROOT/deps/SEAL-4.0.0" -B "$RUN_ROOT/deps/seal-build" \
    -DCMAKE_BUILD_TYPE=Release '-DCMAKE_CXX_FLAGS=-O3 -march=native -mtune=native' \
    -DCMAKE_INSTALL_PREFIX="$RUN_ROOT/deps/seal-install" \
    -DSEAL_USE_MSGSL=OFF -DSEAL_USE_ZLIB=OFF -DSEAL_USE_ZSTD=OFF \
    -DSEAL_USE_INTEL_HEXL=OFF -DSEAL_BUILD_EXAMPLES=OFF \
    -DSEAL_BUILD_TESTS=OFF -DSEAL_BUILD_BENCH=OFF
  cmake --build "$RUN_ROOT/deps/seal-build" -j8
  cmake --install "$RUN_ROOT/deps/seal-build"
) > "$RUN_ROOT/metadata/seal-build.log" 2>&1 &
SEAL_PID=$!
STATUS=0
wait "$HEXL_PID" || STATUS=1
wait "$SEAL_PID" || STATUS=1
if [[ "$STATUS" == 0 ]]; then
  git -C "$RUN_ROOT/deps/hexl-src" rev-parse HEAD > "$RUN_ROOT/metadata/hexl-commit.txt"
  git -C "$RUN_ROOT/deps/SEAL-4.0.0" rev-parse HEAD > "$RUN_ROOT/metadata/seal-commit.txt"
  echo complete > "$RUN_ROOT/metadata/dependencies.status"
else
  echo failed > "$RUN_ROOT/metadata/dependencies.status"
fi
exit "$STATUS"
