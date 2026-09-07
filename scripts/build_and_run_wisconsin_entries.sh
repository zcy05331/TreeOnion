#!/usr/bin/env bash
# Build the revised acceptance policy before restarting the entry-size matrix.
set -euo pipefail
RUN_ROOT="${1:?usage: build_and_run_wisconsin_entries.sh RUN_ROOT DEPENDENCY_RUN_ROOT}"
DEPS_ROOT="${2:?missing dependency run root}"
META="$RUN_ROOT/metadata"
SOURCE="$RUN_ROOT/source"
mkdir -p "$META"
echo "$$" > "$META/controller.pid"
echo building > "$META/status"
trap 'echo failed > "$META/status"; echo 1 > "$META/exit-code.txt"' ERR
cd "$SOURCE"
cmake -S . -B build-native -DCMAKE_BUILD_TYPE=Benchmark \
  -DACTIVE_CONFIG=CONFIG_N2048_K1_COMP \
  -DHEXL_DIR="$DEPS_ROOT/deps/hexl-install/lib64/cmake/hexl-1.2.6" \
  > "$META/configure.log" 2>&1
cmake --build build-native -j4 > "$META/build.log" 2>&1
for TEST in hexl_ntt tree_superrecord_noise tree_superrecord_e2e tree_superrecord_kernel; do
  numactl --physcpubind=0 --interleave=all build-native/Onion-PIR --test "$TEST" \
    > "$META/$TEST.log" 2>&1
done
# exec preserves the controller PID awaited by the later t257 queue.
exec bash scripts/run_wisconsin_entry_sizes.sh "$RUN_ROOT" "$RUN_ROOT"
