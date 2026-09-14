#!/usr/bin/env bash
set -euo pipefail

usage() {
  cat <<'EOF'
Usage: build_spiral_cpu.sh --height 24|26|27 --spiral-root PATH --build-dir PATH [--vcpkg-root PATH] [--dry-run]

Builds the upstream Spiral Seperated CPU target with the checked-in 32-byte
H24/H26/H27 presets.  The script creates a tiny CMake overlay inside --build-dir
so the upstream crypto sources stay untouched while the top-level clang++-12
hardcode is avoided.  Provide vcpkg packages for HEXL, nlohmann_json, simdjson,
and Boost.
EOF
}

height=""
spiral_root=""
build_dir=""
vcpkg_root="${VCPKG_ROOT:-$HOME/vcpkg}"
dry_run=0
jobs="${JOBS:-4}"

while [[ $# -gt 0 ]]; do
  case "$1" in
    --height) height="$2"; shift 2 ;;
    --spiral-root) spiral_root="$2"; shift 2 ;;
    --build-dir) build_dir="$2"; shift 2 ;;
    --vcpkg-root) vcpkg_root="$2"; shift 2 ;;
    --dry-run) dry_run=1; shift ;;
    -h|--help) usage; exit 0 ;;
    *) echo "unknown argument: $1" >&2; usage >&2; exit 2 ;;
  esac
done

if [[ -z "$height" || -z "$spiral_root" || -z "$build_dir" ]]; then
  usage >&2
  exit 2
fi

case "$height" in
  24) build_vars=(TEXP=8 TEXPRIGHT=56 TCONV=4 TGSW=10 QPBITS=20 PVALUE=256 QNUMFIRST=1 QNUMREST=0 OUTN=2 ADDTL_CXX_FLAGS=) ;;
  26) build_vars=(TEXP=8 TEXPRIGHT=56 TCONV=4 TGSW=11 QPBITS=21 PVALUE=256 QNUMFIRST=1 QNUMREST=0 OUTN=2 ADDTL_CXX_FLAGS=) ;;
  27) build_vars=(TEXP=16 TEXPRIGHT=56 TCONV=4 TGSW=11 QPBITS=20 PVALUE=256 QNUMFIRST=1 QNUMREST=0 OUTN=2 ADDTL_CXX_FLAGS=) ;;
  *) echo "height must be 24, 26, or 27" >&2; exit 2 ;;
esac

spiral_root_abs="$(cd "$spiral_root" 2>/dev/null && pwd || true)"
if [[ -z "$spiral_root_abs" ]]; then
  echo "missing Spiral root: $spiral_root" >&2
  exit 1
fi
source_dir="$spiral_root_abs/Seperated"
overlay_dir="$build_dir/source-overlay"
toolchain="$vcpkg_root/scripts/buildsystems/vcpkg.cmake"
compiler="${CXX:-clang++-12}"

configure_cmd=(cmake -G "Unix Makefiles" -S "$overlay_dir" -B "$build_dir" -DCMAKE_BUILD_TYPE=Release -DCMAKE_TOOLCHAIN_FILE="$toolchain" -DCMAKE_CXX_COMPILER="$compiler")
build_cmd=(cmake --build "$build_dir" --target Seperated -j "$jobs" -- "${build_vars[@]}")

printf 'Overlay source:\n  %s\n' "$overlay_dir"
printf 'Configure command:\n  '
printf '%q ' "${configure_cmd[@]}"
printf '\nBuild command:\n  '
printf '%q ' "${build_cmd[@]}"
printf '\nExecutable:\n  %s/Seperated/Seperated\n' "$build_dir"

if [[ "$dry_run" == 1 ]]; then
  exit 0
fi

if [[ ! -f "$source_dir/CMakeLists.txt" ]]; then
  echo "missing Seperated CMakeLists.txt under $source_dir" >&2
  exit 1
fi
if [[ ! -f "$toolchain" ]]; then
  echo "missing vcpkg toolchain at $toolchain" >&2
  exit 1
fi

mkdir -p "$overlay_dir"
ln -sfn "$source_dir" "$overlay_dir/Seperated"
cat > "$overlay_dir/CMakeLists.txt" <<'EOF'
cmake_minimum_required(VERSION 3.12)
project(SpiralSeperatedCpu VERSION 0.1 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_FLAGS_RELEASE "-g -std=c++17 -MD -march=native")
set(CMAKE_CXX_FLAGS_DEBUG "-g -std=c++17 -MD -march=native")
add_compile_options(-O3)
find_package(HEXL CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(simdjson CONFIG REQUIRED)
find_package(Boost 1.83.0 REQUIRED)
add_subdirectory(Seperated)
EOF

"${configure_cmd[@]}"
"${build_cmd[@]}"
