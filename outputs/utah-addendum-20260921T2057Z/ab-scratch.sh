#!/usr/bin/env bash
# A/B for the reused first-dimension scan output buffer: old campaign ep3 binary vs the rebuilt one,
# identical g8/N0 512/split-columns/seed 7 settings, alternating processes at 1 GiB, one pair at 4 GiB.
set -euo pipefail
experiment_root=/users/zcy05331/tree-h1-addendum-20260921T2057Z
old_root=/users/zcy05331/tree-h1-final-20260920T2105Z
cd "$experiment_root"
export OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1 MKL_NUM_THREADS=1 PIR_THREADS=1
export TREE_MULTIPLANE_ENTRY_BITS=256 TREE_MULTIPLANE_G=8 TREE_MULTIPLANE_N0=512 TREE_MULTIPLANE_PACKING=mlwe TREE_MULTIPLANE_QUERY_LAYOUT=split-columns
mkdir -p outputs/ab-scratch
run() { # variant leaves label
  local bin
  if [ "$1" = old ]; then bin="$old_root/build-ep3/Onion-PIR"; else bin="$experiment_root/build-ep3/Onion-PIR"; fi
  python3 wait_idle.py --root "$experiment_root"
  python3 source/scripts/read_process_peak_memory.py --log "outputs/ab-scratch/$3.log" --output "outputs/ab-scratch/$3-process.json" -- \
    numactl --physcpubind=0 --preferred=0 "$bin" --test tree_multiplane_bench --leaf-count "$2" \
    --experiments 50 --warmup 3 --trial-seed 7 --benchmark-json "outputs/ab-scratch/$3.json"
}
for round in 1 2; do
  run old 16777216 "1gib-old-r$round"
  run new 16777216 "1gib-new-r$round"
done
run old 67108864 4gib-old-r1
run new 67108864 4gib-new-r1
