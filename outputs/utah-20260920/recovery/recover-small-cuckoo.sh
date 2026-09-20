#!/usr/bin/env bash
set -euo pipefail
experiment_root=/users/zcy05331/tree-h1-final-20260920T2105Z
cd "$experiment_root"
mkdir -p outputs/core-recovery metadata
printf 'waiting_for_existing_matrix\n' > metadata/cuckoo-small-recovery.status
# The existing matrix owns the machine until its serial controller exits.
# This successor never stops or competes with its experiments.
for attempt in $(seq 1 1440); do
  if ! kill -0 8336 2>/dev/null; then break; fi
  sleep 30
done
if kill -0 8336 2>/dev/null; then
  printf 'wait_timeout\n' > metadata/cuckoo-small-recovery.status
  exit 2
fi
export OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1 MKL_NUM_THREADS=1
export ONIONPIR_BENCH_COMMIT="$(cat metadata/source-commit.txt)"
for scale in 256mb 512mb; do
  if [ "$scale" = 256mb ]; then leaves=4194304; else leaves=8388608; fi
  folder="$experiment_root/outputs/core-matrix/$scale"
  output="$folder/cuckoo_tuned.json"
  if [ -e "$output" ]; then
    printf 'existing_result_requires_inspection\n' > metadata/cuckoo-small-recovery.status
    exit 2
  fi
  printf 'selecting_%s\n' "$scale" > metadata/cuckoo-small-recovery.status
  python3 source/scripts/select_cuckoo_layout.py \
    --output "outputs/core-recovery/$scale-selection.json" --expected-trials 8 \
    --expected-warmups 3 --leaf-count "$leaves" --heights 7 \
    "$folder/tuning/cuckoo/h7.json" > "outputs/core-recovery/$scale-selection.log"
  python3 wait_idle.py --root "$experiment_root"
  printf 'running_%s\n' "$scale" > metadata/cuckoo-small-recovery.status
  BENCHMARK_OUTPUT_JSON="$output" CUCKOO_LAYOUT_HEIGHT=7 \
    python3 source/scripts/read_process_peak_memory.py \
      --log "$folder/cuckoo_tuned.log" --output "$folder/cuckoo_tuned-process.json" -- \
      numactl --physcpubind=0 --preferred=0 "$experiment_root/build-default/Onion-PIR" \
      --test cuckoo_bench --leaf-count "$leaves" --experiments 8 --warmup 3 --trial-seed 7
  python3 source/scripts/summarize_cloudlab_profiles.py --validate "$output" --trials 8 --leaf-count "$leaves"
  python3 - "$experiment_root" "$scale" <<'PY'
import json,sys
from pathlib import Path
root=Path(sys.argv[1]);scale=sys.argv[2];folder=root/'outputs/core-matrix'/scale
status=root/'outputs/core-matrix/status.json';data=json.loads(status.read_text())
data['rows'].append({'scale':scale,'case':'cuckoo_tuned','status':'complete','trials':8,
 'json':str(folder/'cuckoo_tuned.json'),'process':str(folder/'cuckoo_tuned-process.json'),
 'log':str(folder/'cuckoo_tuned.log'),'path_semantics':'legacy_merkle_paths_omit_root',
 'recovery_note':'New formal samples at the feasible reference.h=7; original rejected h8/h9/h10 records preserved'})
data.setdefault('recovered_formal_cases',[]).append(scale+':cuckoo_tuned')
status.write_text(json.dumps(data,indent=2)+'\n')
PY
done
printf 'complete\n' > metadata/cuckoo-small-recovery.status
