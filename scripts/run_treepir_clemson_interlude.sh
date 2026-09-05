#!/usr/bin/env bash
# Insert tuned TreePIR after the running Generic worker and before the held parent.
set -euo pipefail
BASE="${1:?usage: run_treepir_clemson_interlude.sh BASE GENERIC_PID HELD_PARENT_PID}"
GENERIC_PID="${2:?missing Generic worker PID}"
HELD_PARENT_PID="${3:?missing held matrix parent PID}"
RUN_ROOT="$BASE/treepir-backend-tuning-20260905"
SOURCE="$RUN_ROOT/source"
META="$RUN_ROOT/metadata"
mkdir -p "$META" "$RUN_ROOT/deps"
echo "$$" > "$META/controller.pid"
echo waiting_for_generic > "$META/queue.status"
trap 'echo failed > "$META/queue.status"' ERR
require_held_parent() {
  python3 - "$HELD_PARENT_PID" <<'PY_HELD'
from pathlib import Path
import sys
proc=Path('/proc')/sys.argv[1]
state=next(line.split()[1] for line in (proc/'status').read_text().splitlines() if line.startswith('State:'))
command=(proc/'cmdline').read_bytes().replace(b'\0',b' ').decode()
if state != 'T' or 'run_cloudlab_profile_matrix.sh' not in command or 'revision-fb79387' not in command:
    raise RuntimeError('Expected the stopped revision-fb79387 matrix parent; interlude remains stopped')
PY_HELD
}
require_held_parent
python3 - "$GENERIC_PID" <<'PY'
from pathlib import Path
import sys,time
status=Path('/proc')/sys.argv[1]/'status'
while status.exists():
    try:
        if '\nState:\tZ' in status.read_text(): break
    except FileNotFoundError: break
    time.sleep(45)
PY
# The wrapper writes process metadata just after its worker exits.
python3 - "$BASE" <<'PY'
import json,sys,time
from pathlib import Path
base=Path(sys.argv[1])
process=base/'outputs/profiling/8gb/generic-process.json'
for _ in range(30):
    if process.exists(): break
    time.sleep(1)
if json.loads(process.read_text())['exit_code'] != 0:
    raise RuntimeError('Generic worker failed; keep held parent for inspection')
PY
python3 "$SOURCE/scripts/summarize_cloudlab_profiles.py" --validate "$BASE/outputs/profiling/8gb/generic.json" --trials 100 --leaf-count 134217728
export OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1 MKL_NUM_THREADS=1
export SUPERRECORD_MEMORY_BYTES=343597383680
export SUPERRECORD_COMMIT=fb793878713fc017e228af15750a4d247c2ea6d1
ONION="$BASE/revision-fb79387/source/build-native/Onion-PIR"
require_held_parent
echo building_adapters > "$META/queue.status"
if [[ ! -d "$RUN_ROOT/deps/TreePIR/.git" ]]; then
  git clone --shared "$BASE/deps/TreePIR" "$RUN_ROOT/deps/TreePIR"
  git -C "$RUN_ROOT/deps/TreePIR" checkout 930063c5aefc441244abb4890fdf35383f3aa956
  git -C "$RUN_ROOT/deps/TreePIR" apply "$SOURCE/scripts/benchmarks/treepir-scale-prototype.patch"
fi
cmake -S "$RUN_ROOT/deps/TreePIR/VBPIR_TreePIR" -B "$RUN_ROOT/deps/TreePIR/VBPIR_TreePIR/build" \
  -DCMAKE_BUILD_TYPE=Release '-DCMAKE_CXX_FLAGS=-O3 -march=native -mtune=native' \
  -DSEAL_DIR="$BASE/deps/seal-install/lib/cmake/SEAL-4.0" > "$META/vbpir-build.log" 2>&1
cmake --build "$RUN_ROOT/deps/TreePIR/VBPIR_TreePIR/build" -j8 >> "$META/vbpir-build.log" 2>&1
cmake -S "$SOURCE/scripts/treepir_sealpir" -B "$SOURCE/build-treepir-sealpir" \
  -DCMAKE_BUILD_TYPE=Release '-DCMAKE_CXX_FLAGS=-O3 -march=native -mtune=native' \
  -DSEAL_DIR="$BASE/deps/seal-install/lib/cmake/SEAL-4.0" \
  -DSEALPIRPLUS_DIR="$RUN_ROOT/deps/TreePIR/SealPIRplus" > "$META/sealpir-build.log" 2>&1
cmake --build "$SOURCE/build-treepir-sealpir" -j8 >> "$META/sealpir-build.log" 2>&1
TREEPIR_SEALPIR_ADAPTER="$SOURCE/build-treepir-sealpir/treepir_sealpir_adapter" \
  python3 "$SOURCE/scripts/treepir_sealpir/tests/test_treepir_sealpir_adapter.py" > "$META/sealpir-tests.log" 2>&1
printf '0\n1\n1023\n' > "$META/smoke-leaves.txt"
TREEPIR_DATA_DIR="$RUN_ROOT/deps/TreePIR/VBPIR_TreePIR" \
  "$RUN_ROOT/deps/TreePIR/VBPIR_TreePIR/build/bin/vbpir_treepir" --height 10 \
  --leaf-file "$META/smoke-leaves.txt" --warmup 1 --trials 2 --output "$META/vbpir-smoke.json" > "$META/vbpir-smoke.log" 2>&1
echo tuning_and_formal > "$META/queue.status"
python3 "$SOURCE/scripts/run_treepir_backend_tuning.py" --run-root "$RUN_ROOT" \
  --onion-bin "$ONION" --heights 24 --trials 16 --warmups 3 --pilot-trials 16
python3 "$SOURCE/scripts/run_treepir_backend_tuning.py" --run-root "$RUN_ROOT" \
  --onion-bin "$ONION" --heights 27 --trials 100 --warmups 3 --pilot-trials 16
python3 - "$RUN_ROOT" "$BASE" <<'PY'
import json,shutil,sys
from pathlib import Path
root,base=map(Path,sys.argv[1:])
sys.path.insert(0,str(root/'source/scripts'))
from summarize_cloudlab_profiles import validate_profile
for scale,height,trials,stem in [('1gb',24,16,'treepir_tuned'),('8gb',27,100,'treepir')]:
    source=root/'outputs/treepir-backend-tuning'/scale
    data=json.loads((source/'formal.json').read_text())
    if json.loads((source/'formal-process.json').read_text())['exit_code'] != 0:
        raise RuntimeError(f'{scale}: formal process failed')
    validate_profile(data,source/'formal.json',trials,1<<height)
    dest=base/'outputs/profiling'/scale
    for a,b in [('formal.json',stem+'.json'),('formal-process.json',stem+'-process.json'),('formal.log',stem+'.log')]:
        shutil.copyfile(source/a,dest/b)
PY
# The old controller reuses the validated 8 GiB result, then runs its Flat row.
require_held_parent
kill -CONT "$HELD_PARENT_PID"
echo parent_resumed > "$META/queue.status"
