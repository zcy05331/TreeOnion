#!/usr/bin/env bash
set -euo pipefail
# Pass two already-built EP3/drop32 binaries and a fresh output directory.
reference_binary="$1"
candidate_binary="$2"
output_directory="$3"
OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1 MKL_NUM_THREADS=1 \
python3 scripts/benchmark_gadget_configs.py \
  --binary "reference=$reference_binary" --binary "tiled=$candidate_binary" \
  --source reference=e83e3d736be0453476c7fd759916cbce742b27ff \
  --source tiled=806c73aa179c52a2e142e26811655b1247cc2213 \
  --packing reference=mlwe --packing tiled=mlwe \
  --shape 1,8,3,512,5 --expansion-height 10 --leaf-count 4194304 \
  --experiments 16 --rounds 2 --memory-gib 6 --output "$output_directory"
