#!/usr/bin/env python3
"""Build the local probe against the existing x86_64 benchmark objects."""
import json
import shlex
import subprocess
from pathlib import Path

here = Path(__file__).resolve().parent
repo = next(p for p in here.parents if (p / "build-x86_64-benchmark").is_dir())
build = repo / "build-x86_64-benchmark"
link = shlex.split((build / "CMakeFiles/Onion-PIR.dir/link.txt").read_text())
objects = [str(build / p) for p in link if p.endswith(".o")
           and "/tests/" not in p and not p.endswith("/main.cpp.o")]
hexl = next(Path(p) for p in link if p.endswith("/libhexl.a"))
command = ["/usr/bin/arch", "-x86_64", "/usr/bin/clang++", "-std=c++20",
           "-O3", "-arch", "x86_64", "-DACTIVE_CONFIG=CONFIG_N2048_K1_COMP",
           "-DONIONPIR_USE_HEXL", "-D_BENCHMARK", "-I", str(repo / "src/includes"),
           "-isystem", str(hexl.parent.parent / "include"), str(here / "check.cpp"),
           *objects, str(hexl), "-o", str(here / "check")]
subprocess.run(command, check=True)
with (here / "run.log").open("w") as log:
    run = subprocess.run([str(here / "check"), str(here / "results.json")],
                         stdout=log, stderr=subprocess.STDOUT)
if run.returncode:
    raise SystemExit(f"probe exited {run.returncode}; see {here / 'run.log'}")
result = json.loads((here / "results.json").read_text())
assert result["all_correct"] and len(result["checks"]) == 8
assert sum(c["nodes_checked"] for c in result["checks"]) == 768
print(f"PASS: 8 encrypted queries, 768 decoded nodes; {here / 'results.json'}")
