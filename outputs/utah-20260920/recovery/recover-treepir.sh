#!/usr/bin/env bash
set -euo pipefail
experiment_root=/users/zcy05331/tree-h1-final-20260920T2105Z
cd "$experiment_root"
export OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1 MKL_NUM_THREADS=1
trap 'code=$?; if [ "$code" != 0 ]; then printf "failed:%s\n" "$code" > metadata/treepir-recovery.status; fi' EXIT
python3 wait_idle.py --root "$experiment_root"
printf 'validating_indices\n' > metadata/treepir-recovery.status
printf '0\n1\n' > metadata/treepir-gate-leaves.txt
for height in 22 23 24 26; do
 "$experiment_root/deps/TreePIR/VBPIR_TreePIR/build/bin/vbpir_treepir" \
  --height "$height" --leaf-file "$experiment_root/metadata/treepir-gate-leaves.txt" \
  --warmup 1 --trials 1 --validate-index-only --output "$experiment_root/metadata/treepir-index-h$height.json" \
  > "metadata/treepir-index-h$height.log" 2>&1
done
printf 'running\n' > metadata/treepir-recovery.status
printf 'treepir_recovery\n' > metadata/status
python3 "$experiment_root/source/scripts/run_treepir_matrix.py" --root "$experiment_root" > metadata/treepir-recovery.log 2>&1
printf 'complete\n' > metadata/treepir-recovery.status
printf 'finished_with_resource_limits\n' > metadata/status
