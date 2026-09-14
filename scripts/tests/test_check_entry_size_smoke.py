"""Tests for check_entry_size_smoke.py with the benchmark launch stubbed out.

The smoke is the gate run_entry_size_after_matrix.sh and the Wisconsin
entry-size scripts run before the 1 GiB entry-size matrix: it must launch the
small encrypted workload for every default entry size, with the sweep's
environment minus SUPERRECORD_PLAN_ONLY and with the memory cap forced to
2 GiB, and stop on the first failing size without writing validation.json.
"""

import contextlib
import io
import json
import os
import pathlib
import sys
import tempfile
import unittest
from unittest.mock import patch

SCRIPTS = pathlib.Path(__file__).resolve().parents[1]
REPO = SCRIPTS.parent
sys.path.insert(0, str(SCRIPTS))
import check_entry_size_smoke as smoke  # noqa: E402

# kStageLabels of src/tests/test_tree_superrecord_benchmark.cpp
STAGES = ("expand", "complete", "prepare", "scan", "finalize", "fold", "rotate", "project", "pack", "switch", "merge")
SIZES = (16, 48, 64, 128)


def candidate(label, arity, entry_bits, samples=2):
    """Candidate row with the keys write_run_json emits
    (src/tests/test_tree_superrecord_benchmark.cpp)."""
    rows = [
        {
            "block_id": i, "leaf": 11 + i, "server_total_ms": 10.0 + arity,
            "client_query_ms": 1.0, "client_decode_ms": 1.0, "response_serialize_ms": 1.0,
            "min_budget_bits": 20, "max_noise": 30.0, "response_merge_keyswitches": 0,
            "response_payload_bytes": 11264, "real_non_target_neighbour_slots": 0,
            "stages_ms": {stage: 1.0 for stage in STAGES},
        }
        for i in range(samples)
    ]
    return {
        "row_id": f"candidate-{arity}", "label": label, "description": f"bundle arity {arity}",
        "is_generic": True, "is_h1": arity == 1, "mandatory_control": True, "eligible": True,
        "ineligible_reason": None, "setup_ms": 5.0, "preprocess_ms": 5.0, "session_setup_ms": 1.0,
        "noise_gate_ms": 1.0, "query_ms_avg": 1.0, "client_decode_ms_avg": 1.0,
        "wire": {"modeled_seeded_query_bytes": 14880, "actual_query_wire_bytes": None,
                 "actual_response_ciphertext_payload_bytes": 11264},
        "shape": {"entry_bits": entry_bits, "leaf_count": 4096, "bundle_arity": arity,
                  "record_coeffs": 64, "plane_count": 1, "N0": 64, "expansion_height": 6,
                  "response_ciphertexts": 1},
        "candidate_metrics": {"modeled_per_client_eval_key_bytes": 1607040,
                              "optimized_steady_state_bytes_estimate": 1000},
        "target_noise_result": {"fixture_kind": "TargetShapeNoiseGate", "eligibility": "Passed",
                                "decrypts_correctly": True, "observed_key_coverage_matches_tag": True,
                                "min_noise_margin": 1},
        "samples": rows,
    }


def generic_run(entry_bits, samples=2):
    return {
        "schema_version": "tree_superrecord_benchmark.v1",
        "repository_commit": "abc",
        "plan_only": False,
        "run_class": "smoke_only",
        "smoke_only": True,
        "entry_bits": entry_bits,
        "leaf_count": 4096,
        "tree_height": 12,
        "warmup_count": 1,
        "measured_trials": samples,
        "trial_seed": 7,
        "benchmark_bundle_arities": [1, 3],
        "leaf_schedule": {"warmup": [3], "measured": [11 + i for i in range(samples)]},
        "measurement_schedule": "interleaved_blocks",
        "block_execution_orders": (
            [{"warmup": True, "block_id": 0, "leaf": 3, "candidate_indices": [0, 1]}]
            + [{"warmup": False, "block_id": i, "leaf": 11 + i, "candidate_indices": [0, 1]} for i in range(samples)]
        ),
        "candidates": [candidate("generic-h1-mandatory-0", 1, entry_bits, samples),
                       candidate("bundle3", 3, entry_bits, samples)],
    }


class FakeBenchmark:
    """Stands in for read_process_peak_memory.run_measured: records the launch and
    writes a generic run file for the requested entry size."""

    def __init__(self, fail_sizes=()):
        self.calls = []
        self.fail_sizes = set(fail_sizes)

    def __call__(self, command, *, log_path, env=None, cwd=None):
        self.calls.append({"command": list(command), "env": dict(env), "cwd": cwd})
        pathlib.Path(log_path).write_text("fake benchmark\n")
        entry_bits = int(env["SUPERRECORD_ENTRY_BITS"])
        if entry_bits // 8 in self.fail_sizes:
            return {"exit_code": 2, "elapsed_seconds": 0.1, "process_peak_rss_bytes": 1}
        pathlib.Path(env["SUPERRECORD_OUTPUT_JSON"]).write_text(json.dumps(generic_run(entry_bits)) + "\n")
        return {"exit_code": 0, "elapsed_seconds": 0.1, "process_peak_rss_bytes": 1}


class CheckEntrySizeSmokeTest(unittest.TestCase):
    def run_smoke(self, root, fake):
        argv = ["check_entry_size_smoke.py", "--binary", str(root / "Onion-PIR"),
                "--output-dir", str(root / "smoke"), "--source-commit", "abc"]
        # The sweep's env_for() inherits the caller's environment: make the two values the
        # smoke must override actually present, so the assertions below are not vacuous.
        inherited = {"SUPERRECORD_PLAN_ONLY": "1", "SUPERRECORD_MEMORY_BYTES": str(320 * 1024 ** 3)}
        with patch.object(sys, "argv", argv), patch.object(smoke, "run_measured", fake), \
             patch.dict(os.environ, inherited), contextlib.redirect_stdout(io.StringIO()):
            return smoke.main()

    def test_each_default_size_runs_the_small_encrypted_workload(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp).resolve()
            fake = FakeBenchmark()
            self.assertEqual(self.run_smoke(root, fake), 0)
            self.assertEqual([int(call["env"]["SUPERRECORD_ENTRY_BITS"]) // 8 for call in fake.calls], list(SIZES))
            for call in fake.calls:
                self.assertEqual(call["command"], [str(root / "Onion-PIR"), "--test", "tree_superrecord_bench",
                                                   "--leaf-count", "4096", "--experiments", "2",
                                                   "--warmup", "1", "--trial-seed", "7"])
                env = call["env"]
                # Regression: without these two overrides the "smoke" is a plan-only run under
                # the sweep's 320 GiB cap, i.e. no encrypted check happens at all.
                self.assertNotIn("SUPERRECORD_PLAN_ONLY", env)
                self.assertEqual(env["SUPERRECORD_MEMORY_BYTES"], str(2 * 1024 ** 3))
                self.assertEqual(env["SUPERRECORD_COMMIT"], "abc")
                self.assertEqual(env["SUPERRECORD_BUNDLE_ARITIES"], "1,3")
                self.assertEqual(env["SUPERRECORD_SKIP_LEGACY"], "1")
                self.assertEqual(call["cwd"], REPO)
            self.assertEqual([call["env"]["SUPERRECORD_OUTPUT_JSON"] for call in fake.calls],
                             [str(root / "smoke" / f"e{size}b.json") for size in SIZES])

            validation = json.loads((root / "smoke" / "validation.json").read_text())
            self.assertEqual(validation["source_commit"], "abc")
            self.assertEqual([check["entry_bytes"] for check in validation["checks"]], list(SIZES))
            for check in validation["checks"]:
                self.assertTrue(check["passed"])
                self.assertEqual(check["measured_candidates"], 2)
                self.assertEqual((check["leaf_count"], check["samples_per_candidate"], check["warmups"]), (4096, 2, 1))
            for size in SIZES:
                process = json.loads((root / "smoke" / f"e{size}b-process.json").read_text())
                self.assertEqual(process["exit_code"], 0)
                self.assertTrue((root / "smoke" / f"e{size}b.log").exists())

    def test_first_failing_size_stops_the_smoke_without_validation(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp).resolve()
            fake = FakeBenchmark(fail_sizes={48})
            with self.assertRaisesRegex(RuntimeError, "48B"):
                self.run_smoke(root, fake)
            self.assertEqual([int(call["env"]["SUPERRECORD_ENTRY_BITS"]) // 8 for call in fake.calls], [16, 48])
            self.assertEqual(json.loads((root / "smoke" / "e48b-process.json").read_text())["exit_code"], 2)
            self.assertFalse((root / "smoke" / "validation.json").exists())


if __name__ == "__main__":
    unittest.main()
