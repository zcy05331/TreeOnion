#!/usr/bin/env python3
from __future__ import annotations

import argparse
import json
import re
import shutil
import subprocess
from pathlib import Path


REPO = Path(__file__).resolve().parents[3]
DEFAULT_UPSTREAM = REPO.parent / "generic-superrecord" / ".benchmarks" / "TreePIR" / "Spiral"
DEFAULT_HEXL = REPO.parents[1] / "hexl_install"
EXPECTED_TREEPIR_SHA = "930063c5aefc441244abb4890fdf35383f3aa956"


PRESETS = {
    8: {
        "build": {
            "TEXP": "8",
            "TEXPRIGHT": "56",
            "TCONV": "4",
            "TGSW": "8",
            "QPBITS": "20",
            "PVALUE": "256",
            "QNUMFIRST": "1",
            "QNUMREST": "0",
            "OUTN": "2",
            "ADDTL_CXX_FLAGS": "",
        },
        "run": {"num_expansions": "1", "further_dims": "1"},
    },
    10: {
        "build": {
            "TEXP": "8",
            "TEXPRIGHT": "56",
            "TCONV": "4",
            "TGSW": "8",
            "QPBITS": "20",
            "PVALUE": "256",
            "QNUMFIRST": "1",
            "QNUMREST": "0",
            "OUTN": "2",
            "ADDTL_CXX_FLAGS": "",
        },
        "run": {"num_expansions": "1", "further_dims": "1"},
    },
    24: {
        "build": {
            "TEXP": "8",
            "TEXPRIGHT": "56",
            "TCONV": "4",
            "TGSW": "10",
            "QPBITS": "20",
            "PVALUE": "256",
            "QNUMFIRST": "1",
            "QNUMREST": "0",
            "OUTN": "2",
            "ADDTL_CXX_FLAGS": "",
        },
        "run": {"num_expansions": "6", "further_dims": "7"},
    },
    26: {
        "build": {
            "TEXP": "8",
            "TEXPRIGHT": "56",
            "TCONV": "4",
            "TGSW": "11",
            "QPBITS": "21",
            "PVALUE": "256",
            "QNUMFIRST": "1",
            "QNUMREST": "0",
            "OUTN": "2",
            "ADDTL_CXX_FLAGS": "",
        },
        "run": {"num_expansions": "8", "further_dims": "7"},
    },
}


def patch_spiral_root(source: Path) -> None:
    spiral = source / "src" / "spiral.cpp"
    text = spiral.read_text(encoding="utf-8")
    if '#include <cstdlib>\n' not in text:
        text = text.replace('#include "spiral.h"\n', '#include "spiral.h"\n\n#include <cstdlib>\n')
    old_process = 'const std::filesystem::path base("/tmp/Spiral");'
    new_process = (
        'const std::filesystem::path base = [] {\n'
        '        const char *env = std::getenv("TREEPIR_SPIRAL_ROOT");\n'
        '        return std::filesystem::path(env != nullptr && env[0] != \'\\0\' ? env : "/tmp/Spiral");\n'
        '    }();'
    )
    old_eval = 'const std::filesystem::path metricsPath("/tmp/Spiral/Evaluation/Data");'
    new_eval = (
        'const std::filesystem::path metricsPath = [] {\n'
        '        const char *env = std::getenv("TREEPIR_SPIRAL_ROOT");\n'
        '        return (std::filesystem::path(env != nullptr && env[0] != \'\\0\' ? env : "/tmp/Spiral") / "Evaluation/Data");\n'
        '    }();'
    )
    if old_process not in text or old_eval not in text:
        raise RuntimeError("upstream hard-coded /tmp/Spiral snippets were not found")
    text = text.replace(old_process, new_process)
    text = text.replace(old_eval, new_eval)
    text = text.replace('sched_getcpu()', '0')
    wrong_position_check = '(hashExistsInFile ? "correctly." : "incorrectly.")'
    right_position_check = '(hashIsPositionedCorrectly ? "correctly." : "incorrectly.")'
    if wrong_position_check not in text:
        raise RuntimeError("upstream positioned-check print expression was not found")
    text = text.replace(wrong_position_check, right_position_check)
    wrong_decode_assert = "assert(readHeadEnd < databaseCapacity);"
    right_decode_assert = "assert(readHeadEnd <= config.totalCoefficients);"
    if wrong_decode_assert not in text:
        raise RuntimeError("upstream decode bounds assertion was not found")
    text = text.replace(wrong_decode_assert, right_decode_assert)
    simd_body = '''        void useSIMDParser(const std::filesystem::path& jsonFile) {
            simdjson::ondemand::parser simdParser;
            auto jsonHash = simdjson::padded_string::load(jsonFile.c_str());
            auto& hashStoreRef = const_cast<HashStore&>(hashStore(true));
            for (simdjson::ondemand::object container : simdParser.iterate(jsonHash)) {
                for (auto element : container) {
                    simdjson::ondemand::value hash = element.value();
                    hashStoreRef.get<Container::Sequence>().emplace_back(
                            static_cast<std::string_view>(hash.get_string())
                    );
                }
            }
        }
'''
    nlohmann_body = '''        void useSIMDParser(const std::filesystem::path& jsonFile) {
            std::ifstream jsonFileStream(jsonFile);
            nlohmann::ordered_json jsonData;
            jsonFileStream >> jsonData;
            auto& hashStoreRef = const_cast<HashStore&>(hashStore(true));
            for (auto& element : jsonData[0].items()) {
                std::string hash = element.value().get<std::string>();
                hashStoreRef.get<Container::Sequence>().push_back(hash);
            }
        }
'''
    if simd_body not in text:
        raise RuntimeError("upstream simdjson parser block was not found")
    text = text.replace(simd_body, nlohmann_body)
    spiral.write_text(text, encoding="utf-8")
    header = source / "include" / "spiral.h"
    header_text = header.read_text(encoding="utf-8")
    wrong_avx512_alignment = "reoriented_ciphertexts = (uint64_t *)aligned_alloc(32, L_small_size_bytes * n1_padded / n1);"
    right_avx512_alignment = "reoriented_ciphertexts = (uint64_t *)aligned_alloc(64, L_small_size_bytes * n1_padded / n1);"
    if wrong_avx512_alignment not in header_text:
        raise RuntimeError("upstream AVX512 reoriented ciphertext alignment allocation was not found")
    header.write_text(header_text.replace(wrong_avx512_alignment, right_avx512_alignment), encoding="utf-8")
    source_simdjson = source / "include" / "simdjson.h"
    source_simdjson.parent.mkdir(parents=True, exist_ok=True)
    source_simdjson.write_text("#pragma once\n", encoding="utf-8")


def write_overlay(
    overlay: Path,
    staged_source: Path,
    boost_minimum: str,
    simdjson_include: Path,
    extra_include_paths: list[Path],
) -> None:
    overlay.mkdir(parents=True, exist_ok=True)
    stub_include = overlay / "simdjson_stub"
    stub_include.mkdir(parents=True, exist_ok=True)
    (stub_include / "simdjson.h").write_text("#pragma once\n", encoding="utf-8")
    include_lines = "\n".join(
        [f'include_directories(BEFORE "{stub_include}")']
        + [f'include_directories("{path}")' for path in extra_include_paths if path.exists()]
    )
    (overlay / "CMakeLists.txt").write_text(
        f"""\
cmake_minimum_required(VERSION 3.16)
project(TreePirSpiralSeperatedCpu VERSION 0.1 LANGUAGES CXX)
set(CMAKE_CXX_STANDARD 17)
set(CMAKE_CXX_STANDARD_REQUIRED ON)
set(CMAKE_CXX_FLAGS_RELEASE "-g -std=c++17 -MD -march=native")
set(CMAKE_CXX_FLAGS_DEBUG "-g -std=c++17 -MD -march=native")
add_compile_options(-O3)
option(USE_TIMERLOG "Enable Spiral timer metrics" ON)
if(USE_TIMERLOG)
  add_definitions(-DTIMERLOG)
endif()
{include_lines}
find_package(HEXL CONFIG REQUIRED)
find_package(nlohmann_json CONFIG REQUIRED)
find_package(Boost {boost_minimum} REQUIRED)
include_directories(${{Boost_INCLUDE_DIRS}})
add_library(simdjson::simdjson INTERFACE IMPORTED)
set_target_properties(simdjson::simdjson PROPERTIES
  INTERFACE_INCLUDE_DIRECTORIES "{stub_include}")
add_subdirectory("{staged_source}" Seperated)
target_include_directories(Seperated BEFORE PRIVATE "{stub_include}")
""",
        encoding="utf-8",
    )


def existing_paths(paths: list[Path]) -> list[Path]:
    return [path for path in paths if path.exists()]


def split_paths(values: list[str]) -> list[Path]:
    paths: list[Path] = []
    for value in values:
        for part in value.split(":"):
            if part:
                paths.append(Path(part))
    return paths


def default_cxx(hexl_root: Path) -> Path:
    wrapper = hexl_root / "bin" / "x86_64-clang++"
    return wrapper if wrapper.exists() else Path("/usr/bin/c++")


def git_head(path: Path) -> str | None:
    proc = subprocess.run(["git", "-C", str(path), "rev-parse", "HEAD"], text=True, capture_output=True)
    if proc.returncode == 0:
        return proc.stdout.strip()
    return None


def resolve_source_version(upstream_spiral: Path, override: str | None) -> str:
    actual_head = git_head(upstream_spiral) or git_head(upstream_spiral.parent)
    if override is not None:
        if re.fullmatch(r"[0-9a-f]{40}", override) is None:
            raise SystemExit("--source-version must be a full 40-character lowercase hex Git revision")
        if actual_head is not None and override != actual_head:
            raise SystemExit(
                f"--source-version {override} does not match upstream Git HEAD {actual_head}; "
                "only use --source-version for non-git source archives or the matching Git revision"
            )
        return override
    if actual_head is None:
        raise SystemExit("could not determine upstream TreePIR/Spiral git revision; pass --source-version")
    if actual_head != EXPECTED_TREEPIR_SHA:
        raise SystemExit(f"unexpected upstream revision {actual_head}; expected {EXPECTED_TREEPIR_SHA} or pass --source-version")
    return actual_head


def parse_boost_version(configure_log: Path) -> str | None:
    text = configure_log.read_text(encoding="utf-8", errors="replace") if configure_log.exists() else ""
    patterns = [
        r"Found Boost:.*found suitable version \"([^\"]+)\"",
        r"Boost version: ([0-9.]+)",
        r"Found Boost:.*version: ([0-9.]+)",
    ]
    for pattern in patterns:
        match = re.search(pattern, text)
        if match:
            return match.group(1)
    cache = configure_log.parent / "build" / "CMakeCache.txt"
    if cache.exists():
        cache_text = cache.read_text(encoding="utf-8", errors="replace")
        for key in ("Boost_VERSION_STRING", "Boost_VERSION"):
            match = re.search(rf"{key}:[^=]*=([^\n]+)", cache_text)
            if match:
                return match.group(1).strip()
        match = re.search(r"FIND_PACKAGE_MESSAGE_DETAILS_Boost:[^=]*=.*\[v([0-9.]+)", cache_text)
        if match:
            return match.group(1)
    return None


def run(command: list[str], *, cwd: Path, log: Path) -> None:
    log.parent.mkdir(parents=True, exist_ok=True)
    proc = subprocess.run(command, cwd=cwd, text=True, capture_output=True)
    log.write_text(
        "$ " + " ".join(command) + "\n\nSTDOUT:\n" + proc.stdout + "\nSTDERR:\n" + proc.stderr,
        encoding="utf-8",
    )
    if proc.returncode != 0:
        raise RuntimeError(f"command failed ({proc.returncode}); see {log}")


def main() -> int:
    parser = argparse.ArgumentParser(description="Stage and build the upstream TreePIR Spiral Seperated CPU binary.")
    parser.add_argument("--height", type=int, choices=sorted(PRESETS), required=True)
    parser.add_argument("--upstream-spiral", type=Path, default=DEFAULT_UPSTREAM)
    parser.add_argument("--stage-root", type=Path, default=REPO / ".benchmarks" / "spiral-path")
    parser.add_argument("--hexl-root", type=Path, default=DEFAULT_HEXL)
    parser.add_argument("--homebrew-prefix", type=Path, default=Path("/opt/homebrew"))
    parser.add_argument("--conda-prefix", type=Path, default=Path("/opt/homebrew/anaconda3"))
    parser.add_argument("--cxx", type=Path, help="C++ compiler path; defaults to HEXL x86 wrapper when present, otherwise /usr/bin/c++")
    parser.add_argument("--jobs", type=int, default=2)
    parser.add_argument("--boost-minimum", default="1.74")
    parser.add_argument("--prefix-path", action="append", default=[], help="Additional CMake prefix path(s), ':' separated accepted")
    parser.add_argument("--include-path", action="append", default=[], help="Additional header include path(s), ':' separated accepted")
    parser.add_argument("--boost-dir", type=Path, help="Explicit Boost_DIR when a Boost CMake config is available")
    parser.add_argument("--nlohmann-json-dir", type=Path, help="Explicit nlohmann_json_DIR")
    parser.add_argument("--hexl-dir", type=Path, help="Explicit HEXL_DIR")
    parser.add_argument("--simdjson-include", type=Path, help="Directory containing simdjson.h; header-only interface target is used")
    parser.add_argument("--source-version", help="Override upstream TreePIR/Spiral git SHA recorded in the manifest")
    args = parser.parse_args()

    source = args.upstream_spiral / "Seperated"
    if not (source / "src" / "spiral.cpp").exists():
        raise SystemExit(f"missing upstream Seperated source: {source}")
    cxx = args.cxx or default_cxx(args.hexl_root)
    if not cxx.exists():
        raise SystemExit(f"missing compiler: {cxx}")
    source_version = resolve_source_version(args.upstream_spiral, args.source_version)

    stage = args.stage_root / f"h{args.height}"
    staged_source = stage / "source" / "Seperated"
    build_dir = stage / "build"
    overlay = stage / "overlay"
    if staged_source.exists():
        shutil.rmtree(staged_source)
    staged_source.parent.mkdir(parents=True, exist_ok=True)
    shutil.copytree(source, staged_source, symlinks=True)
    patch_spiral_root(staged_source)

    user_prefixes = split_paths(args.prefix_path)
    hexl_dir = args.hexl_dir or args.hexl_root / "lib64" / "cmake" / "hexl-1.2.6"
    nlohmann_json_dir = args.nlohmann_json_dir or args.conda_prefix / "share" / "cmake" / "nlohmann_json"
    local_boost_dir = args.conda_prefix / "lib" / "cmake" / "Boost-1.82.0"
    boost_dir = args.boost_dir or (local_boost_dir if local_boost_dir.exists() else None)
    prefix_path = existing_paths([hexl_dir, nlohmann_json_dir, *( [boost_dir] if boost_dir else [] ), *user_prefixes])
    include_paths = existing_paths([
        *(prefix / "include" for prefix in user_prefixes),
        *split_paths(args.include_path),
        args.conda_prefix / "include",
    ])
    simdjson_include = args.simdjson_include or next((path for path in include_paths if (path / "simdjson.h").exists()), args.homebrew_prefix / "include")
    write_overlay(overlay, staged_source, args.boost_minimum, simdjson_include, include_paths)

    preset = PRESETS[args.height]
    configure = [
        "cmake",
        "-G",
        "Unix Makefiles",
        "-S",
        str(overlay),
        "-B",
        str(build_dir),
        "-DCMAKE_BUILD_TYPE=Release",
        f"-DCMAKE_CXX_COMPILER={cxx}",
        f"-DCMAKE_PREFIX_PATH={';'.join(str(path) for path in prefix_path)}",
        f"-DHEXL_DIR={hexl_dir}",
    ]
    if boost_dir is not None:
        configure.append(f"-DBoost_DIR={boost_dir}")
    if nlohmann_json_dir.exists():
        configure.append(f"-Dnlohmann_json_DIR={nlohmann_json_dir}")
    for prefix in user_prefixes:
        if (prefix / "include").exists():
            configure.append(f"-DBOOST_ROOT={prefix}")
            break
    build = [
        "cmake",
        "--build",
        str(build_dir),
        "--target",
        "Seperated",
        "-j",
        str(args.jobs),
        "--",
        *[f"{key}={value}" for key, value in preset["build"].items()],
    ]
    run(configure, cwd=REPO, log=stage / "configure.log")
    run(build, cwd=REPO, log=stage / "build.log")
    binary = build_dir / "Seperated" / "Seperated"
    manifest = {
        "height": args.height,
        "upstream_spiral": str(args.upstream_spiral),
        "upstream_treepir_commit": source_version,
        "staged_source": str(staged_source),
        "binary": str(binary),
        "preset": preset,
        "configure_command": configure,
        "build_command": build,
        "boost_minimum": args.boost_minimum,
        "boost_version": parse_boost_version(stage / "configure.log"),
        "prefix_path": [str(path) for path in prefix_path],
        "include_paths": [str(path) for path in include_paths],
        "simdjson_include": str(staged_source / "include"),
    }
    (stage / "manifest.json").write_text(json.dumps(manifest, indent=2, sort_keys=True) + "\n", encoding="utf-8")
    print(json.dumps(manifest, indent=2, sort_keys=True))
    return 0


if __name__ == "__main__":
    raise SystemExit(main())
