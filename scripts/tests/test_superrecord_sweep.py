"""Tests for run_superrecord_sweep.py, the documented local sweep driver
(README: scripts/run_superrecord_sweep.sh --leaf-count ... --memory-gib ...).

The benchmark launch (read_process_peak_memory.run_measured) is replaced by a
stub that writes a raw JSON shaped like tree_superrecord_bench's write_run_json,
and the git/sysctl/compiler probes (command_text) return canned strings, so no
binary runs and the provenance is deterministic. The analyzer hand-off is left
real: analyze_superrecord_sweep.py runs as a child process on the stubbed raw.
"""

import contextlib
import io
import json
import os
import pathlib
import subprocess
import sys
import tempfile
import types
import unittest
from unittest.mock import Mock, patch

SCRIPTS = pathlib.Path(__file__).resolve().parents[1]
REPO = SCRIPTS.parent
sys.path.insert(0, str(SCRIPTS))
import run_superrecord_sweep as superrecord  # noqa: E402

STAGES = ("expand", "complete", "prepare", "scan", "finalize", "fold", "rotate", "project", "pack", "switch")
COMMIT = "fb79387000000000000000000000000000000000"
PROVENANCE = {
    "git rev-parse HEAD": COMMIT,
    "git branch --show-current": "codex/test-fixture",
    "git status --porcelain": "",
    "sysctl -n machdep.cpu.brand_string": "Fixture CPU",
    "sysctl -n hw.memsize": "68719476736",
}


def fake_command_text(command):
    """Stands in for run_superrecord_sweep.command_text: canned provenance, no subprocesses."""
    if command[-1] == "--version":
        return "fixture compiler version 1.0"
    return PROVENANCE.get(" ".join(command))


def candidate_row(label, arity, server_ms, leaves):
    """Candidate row with the keys write_run_json emits
    (src/tests/test_tree_superrecord_benchmark.cpp), trimmed to what the analyzer reads."""
    return {
        "row_id": label, "label": label, "description": f"bundle arity {arity}",
        "is_generic": not label.startswith("legacy"), "is_h1": arity == 1,
        "mandatory_control": True, "eligible": True, "ineligible_reason": None,
        "shape": {"bundle_arity": arity, "record_coeffs": 64, "plane_count": 1, "N0": 64,
                  "expansion_height": 6, "response_ciphertexts": 1},
        "candidate_metrics": {"modeled_per_client_eval_key_bytes": 1607040,
                              "optimized_steady_state_bytes_estimate": 1 << 20},
        "wire": {"modeled_seeded_query_bytes": 14880, "actual_query_wire_bytes": None,
                 "actual_response_ciphertext_payload_bytes": 11264},
        "samples": [
            {"block_id": i, "leaf": leaf, "server_total_ms": server_ms + i,
             "client_query_ms": 1.0, "client_decode_ms": 1.0, "response_serialize_ms": 1.0,
             "stages_ms": {stage: 1.0 for stage in STAGES}}
            for i, leaf in enumerate(leaves)
        ],
    }


def benchmark_json(command, env):
    """The SUPERRECORD_OUTPUT_JSON tree_superrecord_bench would write for this launch:
    sample counts follow --experiments/--warmup, a plan-only run has no samples, and the
    legacy-g32 control row is present because the local sweep never sets SUPERRECORD_SKIP_LEGACY."""
    def flag(name):
        return command[command.index(name) + 1]

    trials, warmups, leaves = int(flag("--experiments")), int(flag("--warmup")), int(flag("--leaf-count"))
    plan_only = env.get("SUPERRECORD_PLAN_ONLY") == "1"
    warmup_leaves = [] if plan_only else [3 + i for i in range(warmups)]
    measured = [] if plan_only else [11 + 3 * i for i in range(trials)]
    smoke = trials < 16 or warmups < 3
    return {
        "schema_version": "tree_superrecord_benchmark.v1",
        "repository_commit": env["SUPERRECORD_COMMIT"],
        "plan_only": plan_only,
        "run_class": "plan_only" if plan_only else ("smoke_only" if smoke else "minimum_candidate_selection"),
        "smoke_only": smoke,
        "entry_bits": int(env["SUPERRECORD_ENTRY_BITS"]),
        "leaf_count": leaves,
        "tree_height": leaves.bit_length() - 1,
        "warmup_count": warmups,
        "measured_trials": trials,
        "trial_seed": int(flag("--trial-seed")),
        "execution_seed": int(env["SUPERRECORD_EXECUTION_SEED"]),
        "benchmark_bundle_arities": None,
        "bootstrap_seed": int(env["SUPERRECORD_BOOTSTRAP_SEED"]),
        "leaf_schedule": {"warmup": warmup_leaves, "measured": measured},
        "measurement_schedule": "interleaved_blocks",
        "block_execution_orders": (
            [{"warmup": True, "block_id": i, "leaf": leaf, "candidate_indices": [0, 1, 2]}
             for i, leaf in enumerate(warmup_leaves)]
            + [{"warmup": False, "block_id": i, "leaf": leaf, "candidate_indices": [0, 1, 2]}
               for i, leaf in enumerate(measured)]
        ),
        "candidates": [candidate_row("legacy-g32", 1, 30.0, measured),
                       candidate_row("generic-h1-mandatory-0", 1, 20.0, measured),
                       candidate_row("bundle3", 3, 10.0, measured)],
    }


class FakeBenchmark:
    """Stands in for read_process_peak_memory.run_measured: records the launch, writes the
    log and the raw JSON, and returns a process record shaped like run_measured's."""

    def __init__(self, fail_labels=()):
        self.calls = []
        self.fail_labels = set(fail_labels)        # nonzero exit, no raw file

    def __call__(self, command, *, log_path, env=None, cwd=None):
        self.calls.append({"command": list(command), "env": dict(env), "log_path": pathlib.Path(log_path), "cwd": cwd})
        pathlib.Path(log_path).write_text("fake tree_superrecord_bench\n")
        raw = pathlib.Path(env["SUPERRECORD_OUTPUT_JSON"])
        process = {"exit_code": 0, "elapsed_seconds": 0.25, "process_peak_rss_bytes": 1 << 20,
                   "ru_maxrss_unit": "bytes", "peak_scope": "single benchmark child process",
                   "cgroup_memory_peak_bytes": None, "cgroup_peak_scope": "shared cgroup cumulative peak, when available"}
        if raw.stem in self.fail_labels:
            process["exit_code"] = 1
        else:
            raw.write_text(json.dumps(benchmark_json(command, env), indent=1) + "\n")
        return process


class AnalyzerProbe:
    """Runs the real analyzer child but records its argv and keeps its output off the test log."""

    def __init__(self):
        self.calls = []
        self.completed = []

    def __call__(self, argv, **kwargs):
        self.calls.append(list(argv))
        completed = subprocess.run(argv, **{**kwargs, "capture_output": True, "text": True})
        self.completed.append(completed)
        return completed


def superrecord_env(env):
    return {key: value for key, value in env.items() if key.startswith("SUPERRECORD_")}


class SweepMainTests(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.root = pathlib.Path(tmp.name).resolve()
        self.binary = self.root / "build" / "Onion-PIR"
        self.binary.parent.mkdir()
        self.binary.write_text("")
        self.out = self.root / "sweep"
        self.stderr = io.StringIO()

    def run_sweep(self, fake, *flags, analyzer=None):
        argv = ["run_superrecord_sweep.py", "--binary", str(self.binary), "--output-dir", str(self.out), *flags]
        # Inherited SUPERRECORD_* variables would leak into the env the sweep builds and
        # make the "not set" assertions below depend on the caller's shell.
        clean = {key: value for key, value in os.environ.items() if not key.startswith("SUPERRECORD_")}
        run = analyzer if analyzer is not None else Mock()
        # Replace the module's reference to subprocess, not subprocess.run itself: the latter
        # is global, and environment_metadata's platform.platform() shells out to `uname -p`
        # on macOS, which would land in this stub.
        with patch.object(sys, "argv", argv), patch.object(superrecord, "run_measured", fake), \
             patch.object(superrecord, "command_text", fake_command_text), \
             patch.dict(os.environ, clean, clear=True), \
             patch.object(superrecord, "subprocess", types.SimpleNamespace(run=run)), \
             contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(self.stderr):
            code = superrecord.main()
        self.analyzer_run = run
        return code

    def manifest(self):
        return json.loads((self.out / "manifest.json").read_text())

    def test_smoke_flags_reach_the_benchmark_and_the_raw_is_augmented_and_analyzed(self):
        fake = FakeBenchmark()
        analyzer = AnalyzerProbe()
        code = self.run_sweep(fake, "--smoke", "--leaf-count", "4096", "--memory-gib", "2.5", analyzer=analyzer)

        self.assertEqual(code, 0)
        manifest = self.manifest()
        self.assertEqual(manifest["state"], "complete")
        run, = manifest["runs"]
        self.assertEqual(run["label"], "e256-n4096")
        raw_path = self.out / "e256-n4096.json"
        self.assertEqual(run["raw_file"], str(raw_path))
        # --smoke means 2 measured trials and 1 warmup; --seed is the trial seed.
        self.assertEqual(run["command"], [str(self.binary), "--test", "tree_superrecord_bench",
                                          "--leaf-count", "4096", "--experiments", "2",
                                          "--warmup", "1", "--trial-seed", "7"])
        call, = fake.calls
        self.assertEqual(call["command"], run["command"])
        self.assertEqual(call["cwd"], REPO)
        self.assertEqual(call["log_path"], self.out / "e256-n4096.log")
        # The manifest must record exactly the SUPERRECORD_* environment the benchmark saw.
        env = superrecord_env(call["env"])
        self.assertEqual(run["benchmark_environment"], env)
        self.assertEqual(env, {
            "SUPERRECORD_COMMIT": COMMIT,
            "SUPERRECORD_BOOTSTRAP_SEED": "90304",
            "SUPERRECORD_ENTRY_BITS": "256",
            "SUPERRECORD_OUTPUT_JSON": str(raw_path),
            "SUPERRECORD_MAX_FRONTIER": "2",
            "SUPERRECORD_MAX_ARITY": "8",
            "SUPERRECORD_MAX_RESPONSES": "1",
            "SUPERRECORD_EXECUTION_SEED": "20260904",
            "SUPERRECORD_KEY_MODE": "fixed",
            # --memory-gib is a float and the cap is an integer byte count.
            "SUPERRECORD_MEMORY_BYTES": str(int(2.5 * 1024 ** 3)),
        })

        metadata = json.loads((self.out / "environment.json").read_text())
        self.assertEqual(metadata["repository_commit"], COMMIT)
        self.assertEqual(metadata["binary"], str(self.binary))
        self.assertEqual(manifest["environment"], metadata)
        # The raw run is rewritten with provenance and the process record, keeping its own fields.
        raw = json.loads(raw_path.read_text())
        self.assertEqual(raw["environment"], metadata)
        self.assertEqual(raw["process"], run["process"])
        self.assertEqual(raw["process"]["exit_code"], 0)
        self.assertEqual(raw["entry_bits"], 256)
        self.assertEqual([row["label"] for row in raw["candidates"]], ["legacy-g32", "generic-h1-mandatory-0", "bundle3"])

        # The analyzer is handed the augmented raw and the bootstrap seed (not the trial seed).
        self.assertEqual(analyzer.calls, [[sys.executable, str(REPO / "scripts/analyze_superrecord_sweep.py"),
                                          str(raw_path), "--seed", "90304"]])
        self.assertEqual(run["analysis_exit_code"], 0, analyzer.completed[-1].stderr)
        analysis = json.loads((self.out / "e256-n4096-analysis.json").read_text())
        self.assertEqual(analysis["bootstrap_seed"], 90304)
        self.assertEqual(analysis["measured_blocks"], 2)
        self.assertTrue((self.out / "e256-n4096-analysis.md").exists())

    def test_first_failed_benchmark_stops_the_sweep_before_the_profiles(self):
        fake = FakeBenchmark(fail_labels={"e256-n4096"})
        code = self.run_sweep(fake, "--smoke", "--leaf-count", "4096", "--profiles")

        self.assertEqual(code, 1)
        manifest = self.manifest()
        self.assertEqual(manifest["state"], "failed")
        # Only the failed run is recorded; the nine profile tasks are never launched.
        run, = manifest["runs"]
        self.assertEqual(run["label"], "e256-n4096")
        self.assertEqual(run["process"]["exit_code"], 1)
        self.assertEqual(len(fake.calls), 1)
        self.assertNotIn("analysis_exit_code", run)
        self.analyzer_run.assert_not_called()
        self.assertEqual(list(self.out.glob("*-analysis.*")), [])
        self.assertIn("e256-n4096: failed", self.stderr.getvalue())
        # The log is preserved for inspection.
        self.assertTrue((self.out / "e256-n4096.log").exists())

    def test_profiles_only_plan_only_enumerates_the_fixed_task_list_without_analysis(self):
        fake = FakeBenchmark()
        code = self.run_sweep(fake, "--profiles-only", "--plan-only")

        self.assertEqual(code, 0)
        manifest = self.manifest()
        self.assertEqual(manifest["state"], "complete")
        labels = [run["label"] for run in manifest["runs"]]
        self.assertEqual(labels, ["e13-n256", "e128-n256", "e160-n256", "e192-n256", "e256-n256",
                                  "e384-n256", "e512-n256", "e768-n256", "e30000-n32"])
        self.assertEqual([call["env"]["SUPERRECORD_OUTPUT_JSON"] for call in fake.calls],
                         [str(self.out / f"{label}.json") for label in labels])
        for run, call in zip(manifest["runs"], fake.calls):
            env = run["benchmark_environment"]
            self.assertEqual(env, superrecord_env(call["env"]))
            entry_bits, leaves = run["label"][1:].split("-n")
            self.assertEqual(env["SUPERRECORD_ENTRY_BITS"], entry_bits)
            # Two responses for the 256-leaf profiles, sixteen for the 30000-bit entries.
            self.assertEqual(env["SUPERRECORD_MAX_RESPONSES"], "16" if entry_bits == "30000" else "2")
            self.assertEqual(env["SUPERRECORD_PLAN_ONLY"], "1")
            # Without --smoke the formal defaults apply: 16 trials, 3 warmups.
            self.assertEqual(run["command"][3:], ["--leaf-count", leaves, "--experiments", "16",
                                                  "--warmup", "3", "--trial-seed", "7"])
            self.assertNotIn("analysis_exit_code", run)
            raw = json.loads(pathlib.Path(run["raw_file"]).read_text())
            self.assertTrue(raw["plan_only"])
            self.assertEqual(raw["process"], run["process"])
            self.assertEqual(raw["environment"], manifest["environment"])
        self.analyzer_run.assert_not_called()
        self.assertEqual(list(self.out.glob("*-analysis.*")), [])

    def test_existing_raw_run_is_never_overwritten(self):
        # Re-running into a finished output directory must not clobber measured data.
        self.out.mkdir()
        (self.out / "e256-n4096.json").write_text("{}\n")
        fake = FakeBenchmark()
        with self.assertRaisesRegex(ValueError, "refusing to overwrite"):
            self.run_sweep(fake, "--smoke", "--leaf-count", "4096")
        self.assertEqual(fake.calls, [])
        self.assertEqual((self.out / "e256-n4096.json").read_text(), "{}\n")


class EnvironmentMetadataTests(unittest.TestCase):
    def test_rosetta_wrapper_compiler_and_hexl_come_from_the_configured_build(self):
        # Shape of build-x86_64-benchmark after the documented cmake line: the compiler is
        # /usr/bin/arch with its real arguments only in CMakeCXXCompiler.cmake, not the cache.
        with tempfile.TemporaryDirectory() as tmp:
            build = pathlib.Path(tmp) / "build-x86_64-benchmark"
            (build / "CMakeFiles" / "4.3.0").mkdir(parents=True)
            (build / "CMakeFiles" / "Onion-PIR.dir").mkdir()
            (build / "CMakeCache.txt").write_text("\n".join([
                "# This is the CMakeCache file.",
                "//Build type",
                "CMAKE_BUILD_TYPE:STRING=Benchmark",
                "ACTIVE_CONFIG:UNINITIALIZED=CONFIG_N2048_K1_COMP",
                "CMAKE_CXX_COMPILER:STRING=/usr/bin/arch",
                "CMAKE_CXX_COMPILER-ADVANCED:INTERNAL=1",
                "CMAKE_OSX_ARCHITECTURES:STRING=x86_64",
                "HEXL_DIR:PATH=/opt/hexl_install/lib64/cmake/hexl-1.2.6",
                "",
            ]))
            (build / "CMakeFiles" / "4.3.0" / "CMakeCXXCompiler.cmake").write_text(
                'set(CMAKE_CXX_COMPILER "/usr/bin/arch")\n'
                'set(CMAKE_CXX_COMPILER_ARG1 "-x86_64 /usr/bin/clang++")\n')
            flags = "CXX_FLAGS = -O3 -DNDEBUG -march=x86-64-v3\n"
            (build / "CMakeFiles" / "Onion-PIR.dir" / "flags.make").write_text(flags)
            binary = build / "Onion-PIR"
            binary.write_text("")
            with patch.object(superrecord, "command_text", side_effect=fake_command_text) as probe:
                metadata = superrecord.environment_metadata(binary)

        self.assertEqual(metadata["compiler"], ["/usr/bin/arch", "-x86_64", "/usr/bin/clang++"])
        # The version probe must go through the same wrapper command, not just its first word.
        self.assertIn((( ["/usr/bin/arch", "-x86_64", "/usr/bin/clang++", "--version"],), {}),
                      [(call.args, call.kwargs) for call in probe.call_args_list])
        self.assertEqual(metadata["compiler_version"], "fixture compiler version 1.0")
        self.assertEqual(metadata["compiler_flags"], flags)
        self.assertEqual(metadata["active_config"], "CONFIG_N2048_K1_COMP")
        self.assertEqual(metadata["build_type"], "Benchmark")
        self.assertEqual(metadata["target_architectures"], "x86_64")
        self.assertEqual(metadata["hexl_package_directory"], "/opt/hexl_install/lib64/cmake/hexl-1.2.6")
        self.assertEqual(metadata["hexl_version"], "1.2.6")
        self.assertEqual(metadata["binary"], str(binary))
        self.assertEqual(metadata["repository_commit"], COMMIT)
        self.assertEqual(metadata["branch"], "codex/test-fixture")

    def test_unconfigured_directory_falls_back_to_the_default_compiler(self):
        with tempfile.TemporaryDirectory() as tmp:
            binary = pathlib.Path(tmp) / "Onion-PIR"
            binary.write_text("")
            with patch.object(superrecord, "command_text", side_effect=fake_command_text):
                metadata = superrecord.environment_metadata(binary)

        self.assertEqual(metadata["compiler"], ["/usr/bin/clang++"])
        for key in ("compiler_flags", "active_config", "build_type", "target_architectures",
                    "hexl_package_directory", "hexl_version"):
            self.assertIsNone(metadata[key], key)


if __name__ == "__main__":
    unittest.main()
