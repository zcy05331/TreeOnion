#!/usr/bin/env bash
# Insert the requested Standard baseline at a measured-job boundary.
set -euo pipefail
RUN_ROOT="${1:?usage: run_standard_onion_interlude.sh RUN_ROOT CURRENT_WORKER_PID HELD_PARENT_PID}"
WORKER_PID="${2:?missing current worker PID}"
PARENT_PID="${3:?missing held matrix parent PID}"
SOURCE="$RUN_ROOT/source"
META="$RUN_ROOT/metadata"
BIN="$SOURCE/build-native/Onion-PIR"
mkdir -p "$META"
echo "$$" > "$META/standard-controller.pid"
require_held_parent() {
  python3 - "$PARENT_PID" "$RUN_ROOT" <<'PY'
from pathlib import Path
import sys
proc=Path('/proc')/sys.argv[1]
state=next(line.split()[1] for line in (proc/'status').read_text().splitlines() if line.startswith('State:'))
command=(proc/'cmdline').read_bytes().replace(b'\0',b' ').decode()
if state != 'T' or 'run_cloudlab_profile_matrix.sh' not in command or sys.argv[2] not in command:
    raise RuntimeError('Expected the stopped core matrix parent')
PY
}
require_held_parent
finish() {
  local code=$?
  trap - EXIT
  echo "$code" > "$META/standard.exit-code.txt"
  if [[ "$code" == 0 ]]; then echo complete > "$META/standard.status";
  else echo failed > "$META/standard.status"; fi
  # Do not leave the original independent experiment queue paused on failure.
  if require_held_parent; then kill -CONT "$PARENT_PID"; fi
  exit "$code"
}
trap finish EXIT
echo waiting_for_current_job > "$META/standard.status"
python3 - "$WORKER_PID" <<'PY'
from pathlib import Path
import sys,time
status=Path('/proc')/sys.argv[1]/'status'
while status.exists():
    try:
        if '\nState:\tZ' in status.read_text(): break
    except FileNotFoundError: break
    time.sleep(10)
PY
require_held_parent
cd "$SOURCE"
export OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1 MKL_NUM_THREADS=1
export ONIONPIR_BENCH_COMMIT="$(cat "$META/source-commit.txt")"
export ONIONPIR_BENCH_BRANCH=codex/generic-superrecord
export ONIONPIR_BENCH_CPU="$(lscpu | sed -n 's/^Model name:[[:space:]]*//p')"
export ONIONPIR_BENCH_OS="$(uname -sr)"
export ONIONPIR_BENCH_COMPILER="$(g++ --version | head -n1)"
export ONIONPIR_BENCH_CMAKE="$(cmake --version | head -n1)"
export ONIONPIR_BENCH_HEXL_PATH="$RUN_ROOT/deps/hexl-install/lib64/cmake/hexl-1.2.6"
export ONIONPIR_BENCH_PROCESS_ARCH=x86_64
for SCALE in 1gb 4gb; do
  if [[ "$SCALE" == 1gb ]]; then LEAVES=16777216; else LEAVES=67108864; fi
  FOLDER="$RUN_ROOT/outputs/profiling/$SCALE"
  mkdir -p "$FOLDER"
  # Preserve any earlier completed measurement instead of overwriting it.
  if [[ -e "$FOLDER/standard.json" || -e "$FOLDER/standard-process.json" ]]; then
    echo "Existing Standard output at $FOLDER; inspect before rerunning" >&2
    exit 1
  fi
  echo "running_$SCALE" > "$META/standard.status"
  printf '%s standard\n' "$SCALE" > "$META/current-job.txt"
  printf '%s\tstandard\trunning\t\t%s\n' "$SCALE" "$(date -u +%FT%TZ)" >> "$RUN_ROOT/outputs/profiling/status.tsv"
  python3 scripts/read_process_peak_memory.py \
    --log "$FOLDER/standard.log" --output "$FOLDER/standard-process.json" \
    -- numactl --physcpubind=0 --interleave=all "$BIN" \
    --test merkle_benchmarks --benchmark-case standard_onionpir \
    --leaf-count "$LEAVES" --experiments 100 --warmup 3 --trial-seed 7 \
    --benchmark-json "$FOLDER/standard.json"
  python3 scripts/summarize_cloudlab_profiles.py --validate "$FOLDER/standard.json" --trials 100 --leaf-count "$LEAVES"
  printf '%s\tstandard\tcomplete\t0\t%s\n' "$SCALE" "$(date -u +%FT%TZ)" >> "$RUN_ROOT/outputs/profiling/status.tsv"
done
