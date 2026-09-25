#!/usr/bin/env bash
#SBATCH -J opir-ksp8g
#SBATCH -p secondary
#SBATCH -N 1
#SBATCH --exclusive
#SBATCH --constraint=G8358_100g_256G_NoGPU&intel
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=128
#SBATCH --mem=0
#SBATCH --time=04:00:00
#SBATCH -o /u/zcy331/onionpir-icc-20260922T1837Z/runs/kspir-pbc-8g/slurm-%j.out
set -uo pipefail
base=/u/zcy331/onionpir-icc-20260922T1837Z; name=kspir-pbc-8g; R=$base/runs/$name
mkdir -p "$R/metadata" "$R/outputs"
for l in source deps build-default build-ep3 build-treepir-sealpir; do ln -sfn "$base/$l" "$R/$l"; done
cp "$base/wait_idle.py" "$R/"; cp "$base/metadata/source-commit.txt" "$R/metadata/"
cd "$R"
export OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1 MKL_NUM_THREADS=1 PIR_THREADS=1
export CARGO_HOME="$base/deps/cargo-home" RUSTUP_HOME="$base/deps/rustup-home" PATH="$base/deps/cargo-home/bin:$PATH" RUSTC_BOOTSTRAP=1
export ONIONPIR_BENCH_COMMIT="$(cat metadata/source-commit.txt)" ONIONPIR_BENCH_BRANCH=codex/icc-matrix-via-spiral
export ONIONPIR_BENCH_CPU="$(lscpu | sed -n 's/^Model name:[[:space:]]*//p')" ONIONPIR_BENCH_OS="$(uname -sr)" ONIONPIR_BENCH_PROCESS_ARCH=x86_64
export ONIONPIR_BENCH_COMPILER="$(g++ --version | head -n1)" ONIONPIR_BENCH_HEXL_PATH="$base/deps/hexl-install/lib64/cmake/hexl-1.2.6"
status() { printf '%s\n' "$1" > metadata/status; date -u +%FT%TZ > metadata/updated-utc.txt; }
hostname -s > metadata/node.txt; lscpu > metadata/lscpu.txt; free -g > metadata/memory-before.txt; echo "${SLURM_JOB_ID:-}" > metadata/slurm-job-id.txt
# Configuration gate: identical to the node that ran the Ours rows (ccc0415).
cfg_model="$(lscpu | sed -n 's/^Model name:[[:space:]]*//p')"; cfg_cpus="$(nproc --all)"
cfg_numa="$(lscpu | sed -n 's/^NUMA node(s):[[:space:]]*//p')"; cfg_mem_kib="$(awk '/MemTotal/{print $2}' /proc/meminfo)"
printf '%s\n' "model=$cfg_model cpus=$cfg_cpus numa=$cfg_numa mem_kib=$cfg_mem_kib" > metadata/node-config.txt
if [ "$cfg_model" != "Intel(R) Xeon(R) Platinum 8358 CPU @ 2.60GHz" ] || [ "$cfg_cpus" != 128 ] || [ "$cfg_numa" != 2 ] || [ "$cfg_mem_kib" -lt 250000000 ]; then status wrong_config; exit 4; fi
grep -qw avx512ifma /proc/cpuinfo || { status no_ifma; exit 3; }
failures=0
phase() {
  local label="$1"; shift
  status "$label"
  "$@" > "metadata/$label.log" 2>&1
  local code=$?
  printf '%s\t%s\t%s\n' "$(date -u +%FT%TZ)" "$label" "$code" >> metadata/phase-exits.tsv
  if [ "$code" != 0 ]; then failures=$((failures+1)); fi
  return "$code"
}
# Same-node calibration row, identical in every job, to check the node stays consistent over the chain.
phase calibration python3 source/scripts/run_core_matrix.py --root "$R" --scales 1gb --cases standard \
  --reference-expansion-height 10 --output-dir "$R/outputs/calibration"
K="$base/source/scripts/path_baselines/kspir_pbc"; HX="$base/deps/hexl-install/lib64/cmake/hexl-1.2.6"
phase build_kspir_gcc bash -c "cmake -S $K -B $R/build-gcc -DCMAKE_CXX_COMPILER=g++ -DKSPIR_DIR=$base/deps/kspir -DHEXL_DIR=$HX && cmake --build $R/build-gcc -j\$(nproc)"
# The LLVM-based icpx build; its runtime libraries come from the same environment script.
set +u; source /sw/apps/intel/oneapi/compiler/2025.0/env/vars.sh > /dev/null 2>&1; set -u  # vars.sh reads unset variables
icpx --version | head -1 > metadata/icpx-version.txt; g++ --version | head -1 > metadata/gcc-version.txt
phase build_kspir_icpx bash -c "cmake -S $K -B $R/build-icpx -DCMAKE_CXX_COMPILER=icpx -DKSPIR_DIR=$base/deps/kspir -DHEXL_DIR=$HX && cmake --build $R/build-icpx -j\$(nproc)"
for v in gcc icpx; do
  phase self_test_$v "$R/build-$v/kspir_pbc" --self-test --fixture "$K/tests/fixtures/cpp_h22_bucket_member_counts.json"
done
phase kspir_pbc_matrix python3 source/scripts/run_kspir_pbc_matrix.py --root "$R" --heights 27 \
  --binary "gcc=$R/build-gcc/kspir_pbc" --binary "icpx=$R/build-icpx/kspir_pbc"
if [ "$failures" != 0 ]; then status complete_with_failures; else status complete; fi
