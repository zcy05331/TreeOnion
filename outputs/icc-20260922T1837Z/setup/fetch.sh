#!/usr/bin/env bash
# Login node only: network fetches and light configuration, no compilation.
set -euo pipefail
root=/u/zcy331/onionpir-icc-20260922T1837Z
cd "$root"
status() { printf '%s\n' "$1" > metadata/status; date -u +%FT%TZ > metadata/updated-utc.txt; }
trap 'code=$?; if [ "$code" != 0 ]; then status fetch_failed; fi' EXIT
clone_pin() {
  local name="$1" url="$2" rev="$3"
  [ -d "deps/$name/.git" ] || git init -q "deps/$name"
  git -C "deps/$name" remote get-url origin >/dev/null 2>&1 || git -C "deps/$name" remote add origin "$url"
  git -C "deps/$name" fetch -q --depth=1 origin "$rev"
  git -C "deps/$name" checkout -q --detach FETCH_HEAD
  git -C "deps/$name" rev-parse HEAD > "metadata/$name-commit.txt"
}
status fetching_sources
clone_pin TreePIR https://github.com/PIR-PIXR/TreePIR.git 930063c5aefc441244abb4890fdf35383f3aa956
clone_pin respire https://github.com/AMACB/respire.git fd20d38e9e94c0298ab21af2f7699a23729eef67
clone_pin SEAL-4.0.0 https://github.com/cnquang/SEAL-4.0.0.git 7e21ef9353ef8e6c5e1f646c9ed8ff486d2ddb8c
clone_pin VIA https://github.com/owniai/VIA.git f65aa9de14ea49fef3bc090885a0db863f65557d
clone_pin spiral-rs https://github.com/menonsamir/spiral-rs.git 335aebf9f5a487f46a1f20ad2f4300618ad24628
git -C deps/TreePIR apply "$root/source/scripts/benchmarks/treepir-scale-prototype.patch"
status installing_rust
export CARGO_HOME="$root/deps/cargo-home" RUSTUP_HOME="$root/deps/rustup-home"
curl --proto '=https' --tlsv1.2 -sSf https://sh.rustup.rs -o deps/rustup-init.sh
sh deps/rustup-init.sh -y --no-modify-path --profile minimal --default-toolchain 1.98.1 > metadata/rust-install.log 2>&1
"$CARGO_HOME/bin/rustc" --version > metadata/rustc.txt
export PATH="$CARGO_HOME/bin:$PATH" RUSTC_BOOTSTRAP=1
status staging_respire
RESPIRE_SOURCE_DIR="$root/deps/respire" bash source/scripts/path_baselines/respire/stage_respire.sh > metadata/respire-stage.log 2>&1
cargo fetch --locked --manifest-path source/.benchmarks/respire-path/respire/Cargo.toml > metadata/cargo-fetch-respire.log 2>&1
cargo fetch --manifest-path deps/spiral-rs/Cargo.toml > metadata/cargo-fetch-spiral.log 2>&1 || true
status sources_ready
