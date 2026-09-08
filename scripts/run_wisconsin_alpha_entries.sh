#!/usr/bin/env bash
# Native preparation on the new sm110p, followed by the serial measured matrix.
set -euo pipefail
RUN_ROOT="${1:?usage: run_wisconsin_alpha_entries.sh RUN_ROOT}"
SOURCE="$RUN_ROOT/source"
META="$RUN_ROOT/metadata"
mkdir -p "$META" "$RUN_ROOT/outputs" "$RUN_ROOT/deps"
echo "$$" > "$META/controller.pid"
echo building > "$META/status"
echo '{"status":"running","current_stage":"building","finished_stages":[],"stages":{}}' > "$META/progress.json"
fail() {
  python3 - "$RUN_ROOT" <<'PY'
from pathlib import Path
import json,sys
r=Path(sys.argv[1]);p=r/'metadata/progress.json'
d=json.loads(p.read_text());d['status']='failed';d['error']='build or preflight failed; see metadata logs'
if (r/'outputs/preflight').exists(): d['finished_stages']=['preflight']
p.write_text(json.dumps(d,indent=2)+'\n');(r/'metadata/status').write_text('failed\n')
PY
}
trap fail ERR
export OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1 MKL_NUM_THREADS=1 PYTHONUNBUFFERED=1
git clone https://github.com/intel/hexl.git "$RUN_ROOT/deps/hexl-src" > "$META/hexl-fetch.log" 2>&1
git -C "$RUN_ROOT/deps/hexl-src" checkout 75a60b8cc908fb4c166315dd8b55b2e1ee7faea7 >> "$META/hexl-fetch.log" 2>&1
cmake -S "$RUN_ROOT/deps/hexl-src" -B "$RUN_ROOT/deps/hexl-build" \
  -DCMAKE_BUILD_TYPE=Release -DCMAKE_INSTALL_PREFIX="$RUN_ROOT/deps/hexl-install" \
  -DCMAKE_INSTALL_LIBDIR=lib64 -DHEXL_BENCHMARK=OFF -DHEXL_TESTING=OFF \
  > "$META/hexl-build.log" 2>&1
cmake --build "$RUN_ROOT/deps/hexl-build" -j4 >> "$META/hexl-build.log" 2>&1
cmake --install "$RUN_ROOT/deps/hexl-build" >> "$META/hexl-build.log" 2>&1
git -C "$RUN_ROOT/deps/hexl-src" rev-parse HEAD > "$META/hexl-commit.txt"
HEXL_DIR="$RUN_ROOT/deps/hexl-install/lib64/cmake/hexl-1.2.6"
for VARIANT in baseline optimized t257; do
  SRC="$SOURCE"; BUILD="$SOURCE/build-native"; CONFIG=CONFIG_N2048_K1_COMP
  if [[ "$VARIANT" == baseline ]]; then SRC="$RUN_ROOT/baseline-source"; BUILD="$SRC/build-native"; fi
  if [[ "$VARIANT" == t257 ]]; then BUILD="$SOURCE/build-t257"; CONFIG=CONFIG_N2048_K1_COMP_T257; fi
  cmake -S "$SRC" -B "$BUILD" -DCMAKE_BUILD_TYPE=Benchmark \
    -DACTIVE_CONFIG="$CONFIG" -DHEXL_DIR="$HEXL_DIR" > "$META/configure-$VARIANT.log" 2>&1
  cmake --build "$BUILD" -j4 > "$META/build-$VARIANT.log" 2>&1
  numactl --physcpubind=0 --interleave=all "$BUILD/Onion-PIR" --test hexl_ntt \
    > "$META/$VARIANT-hexl_ntt.log" 2>&1
done
cd "$SOURCE"
numactl --physcpubind=0 --interleave=all bash scripts/run_regression.sh build-native/Onion-PIR \
  > "$META/optimized-regression.log" 2>&1
numactl --physcpubind=0 --interleave=all "$RUN_ROOT/baseline-source/build-native/Onion-PIR" \
  --test all --experiments 3 --warmup 1 > "$META/baseline-regression.log" 2>&1
numactl --physcpubind=0 --interleave=all build-t257/Onion-PIR --test tree_g32 \
  > "$META/t257-tree_g32.log" 2>&1
numactl --physcpubind=0 --interleave=all python3 scripts/check_entry_size_smoke.py \
  --binary build-native/Onion-PIR --source-commit "$(cat "$META/source-commit.txt")" \
  --output-dir "$RUN_ROOT/outputs/preflight" > "$META/entry-smoke.log" 2>&1
exec numactl --physcpubind=0 --interleave=all python3 scripts/run_wisconsin_alpha_entries.py --run-root "$RUN_ROOT"
