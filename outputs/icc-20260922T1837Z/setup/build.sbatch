#!/usr/bin/env bash
#SBATCH -J opir-build
#SBATCH -p secondary
#SBATCH -N 1
#SBATCH --exclusive
#SBATCH --constraint=G8358&NoGPU
#SBATCH --mem=0
#SBATCH --time=02:00:00
#SBATCH -o /u/zcy331/onionpir-icc-20260922T1837Z/metadata/slurm-build-%j.out
set -euo pipefail
root=/u/zcy331/onionpir-icc-20260922T1837Z
cd "$root"
export OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1 MKL_NUM_THREADS=1 PIR_THREADS=1
export CARGO_HOME="$root/deps/cargo-home" RUSTUP_HOME="$root/deps/rustup-home" PATH="$root/deps/cargo-home/bin:$PATH" RUSTC_BOOTSTRAP=1
status() { printf '%s\n' "$1" > metadata/status; date -u +%FT%TZ > metadata/updated-utc.txt; }
trap 'code=$?; if [ "$code" != 0 ]; then status "build_failed_$(cat metadata/status)"; fi' EXIT
J=$(nproc)
pin() { numactl --physcpubind=0 --preferred=0 "$@"; }
status recording_node
hostname > metadata/build-node.txt; lscpu > metadata/lscpu.txt; numactl --hardware > metadata/numa.txt; free -h > metadata/memory.txt; g++ --version | head -1 > metadata/compiler.txt
grep -o -w 'avx512f\|avx512dq\|avx512ifma\|avx512vbmi' /proc/cpuinfo | sort -u | tr '\n' ' ' > metadata/cpu-flags.txt
grep -qw avx512ifma /proc/cpuinfo || { status no_ifma; exit 3; }
status building_hexl
cmake --build deps/hexl-build -j$J > metadata/hexl-build.log 2>&1
cmake --install deps/hexl-build > metadata/hexl-install.log 2>&1
for v in default ep3; do
  status building_$v
  opts=(); if [ $v = ep3 ]; then opts=(-DPIR_EP_ROWS=3 -DPIR_EP_LOW_BITS=32); fi
  cmake -S source -B build-$v -DCMAKE_BUILD_TYPE=Benchmark -DACTIVE_CONFIG=CONFIG_N2048_K1_COMP \
    -DHEXL_DIR=$root/deps/hexl-install/lib64/cmake/hexl-1.2.6 "${opts[@]}" > metadata/configure-$v.log 2>&1
  cmake --build build-$v -j$J > metadata/build-$v.log 2>&1
done
status correctness_default
pin build-default/Onion-PIR --test all --experiments 3 --warmup 1 > metadata/correctness-default.log 2>&1
status correctness_ep3
pin build-ep3/Onion-PIR --test tree_multiplane > metadata/correctness-ep3.log 2>&1
pin build-ep3/Onion-PIR --test tree_noise_envelope > metadata/noise-ep3.log 2>&1
status building_seal
cmake -S deps/SEAL-4.0.0 -B deps/seal-build -DCMAKE_BUILD_TYPE=Release '-DCMAKE_CXX_FLAGS=-O3 -march=native -mtune=native' \
  -DCMAKE_INSTALL_PREFIX=$root/deps/seal-install -DSEAL_USE_MSGSL=OFF -DSEAL_USE_ZLIB=OFF -DSEAL_USE_ZSTD=OFF \
  -DSEAL_USE_INTEL_HEXL=ON -DHEXL_DIR=$root/deps/hexl-install/lib64/cmake/hexl-1.2.6 \
  -DSEAL_BUILD_EXAMPLES=OFF -DSEAL_BUILD_TESTS=OFF -DSEAL_BUILD_BENCH=OFF > metadata/seal-configure.log 2>&1
cmake --build deps/seal-build -j$J > metadata/seal-build.log 2>&1
cmake --install deps/seal-build > metadata/seal-install.log 2>&1
status building_treepir
cmake -S deps/TreePIR/VBPIR_TreePIR -B deps/TreePIR/VBPIR_TreePIR/build -DCMAKE_BUILD_TYPE=Release \
  '-DCMAKE_CXX_FLAGS=-O3 -march=native -mtune=native' -DSEAL_DIR=$root/deps/seal-install/lib/cmake/SEAL-4.0 > metadata/treepir-configure.log 2>&1
cmake --build deps/TreePIR/VBPIR_TreePIR/build -j$J > metadata/treepir-build.log 2>&1
cmake -S source/scripts/treepir_sealpir -B build-treepir-sealpir -DCMAKE_BUILD_TYPE=Release \
  '-DCMAKE_CXX_FLAGS=-O3 -march=native -mtune=native' -DSEAL_DIR=$root/deps/seal-install/lib/cmake/SEAL-4.0 \
  -DSEALPIRPLUS_DIR=$root/deps/TreePIR/SealPIRplus > metadata/sealpir-configure.log 2>&1
cmake --build build-treepir-sealpir -j$J > metadata/sealpir-build.log 2>&1
status testing_treepir
TREEPIR_SEALPIR_ADAPTER=$root/build-treepir-sealpir/treepir_sealpir_adapter python3 source/scripts/treepir_sealpir/tests/test_treepir_sealpir_adapter.py > metadata/sealpir-tests.log 2>&1
printf '0\n1\n' > metadata/treepir-gate-leaves.txt
for h in 22 23 24 25 26; do
  deps/TreePIR/VBPIR_TreePIR/build/bin/vbpir_treepir --height $h --leaf-file metadata/treepir-gate-leaves.txt \
    --warmup 1 --trials 1 --validate-index-only --output metadata/treepir-index-h$h.json > metadata/treepir-index-h$h.log 2>&1
done
status building_respire
CARGO_TARGET_DIR=$root/source/.benchmarks/respire-path/cargo-target-external RUSTFLAGS='-C target-cpu=native' \
  cargo build --offline --locked --release --manifest-path source/.benchmarks/respire-path/respire/Cargo.toml --bin path_workload > metadata/respire-build.log 2>&1
status testing_respire
(cd source && RESPIRE_PATH_ADAPTER_BIN=$root/source/.benchmarks/respire-path/cargo-target-external/release/path_workload \
  python3 -m unittest scripts.tests.test_respire_path_adapter -v) > metadata/respire-tests.log 2>&1
status build_ready
