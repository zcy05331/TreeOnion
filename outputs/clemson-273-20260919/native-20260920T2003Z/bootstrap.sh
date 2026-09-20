#!/usr/bin/env bash
set -euo pipefail
experiment_root=/users/zcy05331/tree-h1-old-layout-20260920T2003Z
cd "$experiment_root"
mkdir -p metadata outputs source
status() { printf '%s\n' "$1" > metadata/status; date -u +%FT%TZ > metadata/updated-utc.txt; }
trap 'code=$?; if [ "$code" != 0 ]; then status failed; printf "%s\n" "$code" > metadata/exit-code; fi' EXIT
tar -xzf source.tar.gz -C source
lscpu > metadata/lscpu.txt
numactl --hardware > metadata/numa.txt
free -h > metadata/memory-before.txt
g++ --version > metadata/compiler.txt
for variant in default ep3; do
  status waiting_idle_before_build_$variant
  python3 wait_idle.py --root "$experiment_root"
  options=()
  if [ "$variant" = ep3 ]; then options=(-DPIR_EP_ROWS=3 -DPIR_EP_LOW_BITS=32); fi
  status building_$variant
  cmake -S source -B "build-$variant" -DCMAKE_BUILD_TYPE=Benchmark \
    -DACTIVE_CONFIG=CONFIG_N2048_K1_COMP \
    -DHEXL_DIR=/users/zcy05331/onion-cuckoo-rs-20260919T234720Z/deps/hexl-install/lib64/cmake/hexl-1.2.6 \
    "${options[@]}" > "metadata/configure-$variant.log" 2>&1
  cmake --build "build-$variant" -j16 > "metadata/build-$variant.log" 2>&1
  status waiting_idle_before_correctness_$variant
  python3 wait_idle.py --root "$experiment_root"
  status correctness_$variant
  if [ "$variant" = default ]; then
    numactl --physcpubind=0 --preferred=0 "$experiment_root/build-default/Onion-PIR" \
      --test all --experiments 3 --warmup 1 > metadata/correctness-default.log 2>&1
  else
    numactl --physcpubind=0 --preferred=0 "$experiment_root/build-ep3/Onion-PIR" \
      --test tree_multiplane > metadata/correctness-ep3.log 2>&1
    TREE_MULTIPLANE_G=8 TREE_MULTIPLANE_N0=4 TREE_MULTIPLANE_QUERY_LAYOUT=split-columns \
      numactl --physcpubind=0 --preferred=0 "$experiment_root/build-ep3/Onion-PIR" \
      --test tree_multiplane_bench --leaf-count 4096 --experiments 3 --warmup 3 \
      --trial-seed 7 --benchmark-json "$experiment_root/metadata/smoke-ep3.json" \
      > metadata/smoke-ep3.log 2>&1
  fi
done
status waiting_for_finite_queue
for attempt in $(seq 1 180); do
  if [ -f metadata/queue-ready ]; then exec bash "$experiment_root/run-matrix.sh"; fi
  sleep 10
done
status queue_not_ready
exit 2
