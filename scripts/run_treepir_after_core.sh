#!/usr/bin/env bash
# Finish the explicitly deferred TreePIR rows after the core queue releases CPU/RAM.
set -euo pipefail
RUN_ROOT="${1:?usage: run_treepir_after_core.sh RUN_ROOT CORE_CONTROLLER_PID}"
PREDECESSOR_PID="${2:?missing core controller PID}"
META="$RUN_ROOT/metadata"
SOURCE="$RUN_ROOT/source"
mkdir -p "$META"
echo "$$" > "$META/treepir-controller.pid"
echo waiting_for_core > "$META/treepir-queue.status"
trap 'echo failed > "$META/treepir-queue.status"' ERR
python3 - "$PREDECESSOR_PID" <<'PY'
from pathlib import Path
import sys,time
status = Path('/proc') / sys.argv[1] / 'status'
while status.exists():
    try:
        if '\nState:\tZ' in status.read_text():
            break
    except FileNotFoundError:
        break
    time.sleep(45)
PY
cd "$SOURCE"
export OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1 MKL_NUM_THREADS=1
export SUPERRECORD_MEMORY_BYTES=118111600640
export SUPERRECORD_COMMIT="$(cat "$META/source-commit.txt")"
echo building_adapter > "$META/treepir-queue.status"
cmake -S scripts/treepir_sealpir -B build-treepir-sealpir \
  -DCMAKE_BUILD_TYPE=Release '-DCMAKE_CXX_FLAGS=-O3 -march=native -mtune=native' \
  -DSEAL_DIR="$RUN_ROOT/deps/seal-install/lib/cmake/SEAL-4.0" \
  -DSEALPIRPLUS_DIR="$RUN_ROOT/deps/TreePIR/SealPIRplus" \
  > "$META/sealpir-adapter-build.log" 2>&1
cmake --build build-treepir-sealpir -j8 >> "$META/sealpir-adapter-build.log" 2>&1
TREEPIR_SEALPIR_ADAPTER="$SOURCE/build-treepir-sealpir/treepir_sealpir_adapter" \
  python3 scripts/treepir_sealpir/tests/test_treepir_sealpir_adapter.py \
  > "$META/sealpir-adapter-tests.log" 2>&1
echo tuning_and_formal > "$META/treepir-queue.status"
python3 scripts/run_treepir_backend_tuning.py --run-root "$RUN_ROOT" \
  --heights 24 26 --trials 16 --warmups 3 --pilot-trials 16
python3 - "$RUN_ROOT" <<'PY_PROMOTE'
import json,shutil,sys
from datetime import datetime,timezone
from pathlib import Path
root = Path(sys.argv[1])
sys.path.insert(0, str(root / 'source' / 'scripts'))
from summarize_cloudlab_profiles import validate_profile
for scale,height in [('1gb',24),('4gb',26)]:
    formal = root / 'outputs' / 'treepir-backend-tuning' / scale
    raw = formal / 'formal.json'
    data = json.loads(raw.read_text())
    process = json.loads((formal / 'formal-process.json').read_text())
    if process['exit_code'] != 0:
        raise RuntimeError(f'{scale}: selected formal process failed')
    validate_profile(data, raw, 16, 1 << height)
    destination = root / 'outputs' / 'profiling' / scale
    for source_name,target_name in [('formal.json','treepir.json'),
                                    ('formal-process.json','treepir-process.json'),
                                    ('formal.log','treepir.log')]:
        shutil.copyfile(formal / source_name, destination / target_name)
    with (root / 'outputs' / 'profiling' / 'status.tsv').open('a') as stream:
        stamp = datetime.now(timezone.utc).isoformat()
        stream.write(f'{scale}\ttreepir\tcomplete\t0\t{stamp}\n')
PY_PROMOTE
python3 scripts/summarize_cloudlab_profiles.py --directory "$RUN_ROOT/outputs/profiling"
echo 0 > "$META/matrix-pending.txt"
if [[ "$(cat "$META/matrix-failures.txt")" == 0 ]]; then
  echo complete > "$META/matrix.status"
fi
echo complete > "$META/treepir-queue.status"
