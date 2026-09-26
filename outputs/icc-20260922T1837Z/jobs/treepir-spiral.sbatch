#!/usr/bin/env bash
#SBATCH -J opir-tspiral
#SBATCH -p secondary
#SBATCH -N 1
#SBATCH --exclusive
#SBATCH --constraint=G8358_100g_256G_NoGPU&intel
#SBATCH --ntasks=1
#SBATCH --cpus-per-task=128
#SBATCH --mem=0
#SBATCH --time=03:00:00
#SBATCH -o /u/zcy331/onionpir-icc-20260922T1837Z/runs/treepir-spiral/slurm-%j.out
set -uo pipefail
base=/u/zcy331/onionpir-icc-20260922T1837Z; name=treepir-spiral; R=$base/runs/$name
mkdir -p "$R/metadata" "$R/outputs"
for l in source deps build-default build-ep3 build-treepir-sealpir; do ln -sfn "$base/$l" "$R/$l"; done
cp "$base/wait_idle.py" "$R/"; cp "$base/metadata/source-commit.txt" "$R/metadata/"
# The TreePIR row's formal results (colors, indices, leaves): treepir to 4 GiB, treepir-8g at 8 GiB.
mkdir -p "$R/treepir-formal"
for s in 256mb 512mb 1gb 2gb 4gb; do ln -sfn "$base/runs/treepir/outputs/treepir-backend-tuning/$s" "$R/treepir-formal/$s"; done
ln -sfn "$base/runs/treepir-8g/outputs/treepir-backend-tuning/8gb" "$R/treepir-formal/8gb"
cd "$R"
export OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1 MKL_NUM_THREADS=1 PIR_THREADS=1
export CARGO_HOME="$base/deps/cargo-home" RUSTUP_HOME="$base/deps/rustup-home" PATH="$base/deps/cargo-home/bin:$PATH" RUSTC_BOOTSTRAP=1
export ONIONPIR_BENCH_COMMIT="$(cat metadata/source-commit.txt)" ONIONPIR_BENCH_BRANCH=codex/treepir-spiral
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
phase build_spiral env CARGO_TARGET_DIR="$R/build-cargo" RUSTFLAGS="-C target-cpu=native" \
  cargo build --offline --locked --release --manifest-path source/scripts/path_baselines/spiral_pbc/Cargo.toml
phase treepir_spiral_matrix python3 source/scripts/run_spiral_matrix.py --root "$R" --scheme treepir_spiral \
  --treepir-root "$R/treepir-formal" --heights 22 23 24 25 26 27 --binary "$R/build-cargo/release/treepir_spiral"
if [ "$failures" != 0 ]; then status complete_with_failures; else status complete; fi
