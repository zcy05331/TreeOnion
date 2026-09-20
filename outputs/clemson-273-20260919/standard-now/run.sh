#!/usr/bin/env bash
set -euo pipefail
experiment_root=/users/zcy05331/standard-onion-1gb-20260920T0132Z
cd "$experiment_root"
mkdir -p metadata outputs/1gb source
status() { printf '%s\n' "$1" > metadata/status; date -u +%FT%TZ > metadata/updated-utc.txt; }
trap 'code=$?; if [ "$code" != 0 ]; then status failed; printf "%s\n" "$code" > metadata/exit-code; fi' EXIT
status waiting_idle
python3 wait_idle.py --root "$experiment_root"
tar -xzf source.tar.gz -C source
lscpu > metadata/lscpu.txt
numactl --hardware > metadata/numa.txt
free -h > metadata/memory-before.txt
g++ --version > metadata/compiler.txt
status configuring
cmake -S source -B build-default -DCMAKE_BUILD_TYPE=Benchmark \
  -DACTIVE_CONFIG=CONFIG_N2048_K1_COMP \
  -DHEXL_DIR=/users/zcy05331/onion-cuckoo-rs-20260919T234720Z/deps/hexl-install/lib64/cmake/hexl-1.2.6 \
  > metadata/configure.log 2>&1
status building
cmake --build build-default -j16 > metadata/build.log 2>&1
status waiting_idle
python3 wait_idle.py --root "$experiment_root"
status correctness
numactl --physcpubind=0 --preferred=0 build-default/Onion-PIR --test all --experiments 3 --warmup 1 > metadata/correctness.log 2>&1
status waiting_idle
python3 wait_idle.py --root "$experiment_root"
status running_standard_1gb
OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1 MKL_NUM_THREADS=1 \
python3 source/scripts/read_process_peak_memory.py \
  --log outputs/1gb/standard.log --output outputs/1gb/standard-process.json -- \
  numactl --physcpubind=0 --preferred=0 "$experiment_root/build-default/Onion-PIR" \
  --test merkle_benchmarks --benchmark-case standard_onionpir \
  --leaf-count 16777216 --experiments 50 --warmup 3 --trial-seed 7 \
  --benchmark-json "$experiment_root/outputs/1gb/standard.json"
status validating
python3 source/scripts/summarize_cloudlab_profiles.py --validate outputs/1gb/standard.json --trials 50 --leaf-count 16777216 > metadata/validation.log 2>&1
free -h > metadata/memory-after.txt
status complete
