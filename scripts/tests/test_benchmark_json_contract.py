#!/usr/bin/env python3
"""End-to-end contract between the benchmark JSON writers in C++ and their
Python readers, run against the real Onion-PIR binary.

Every other test in scripts/tests hand-writes its benchmark fixtures and mocks
subprocess, so a rename or a statistics regression inside
src/tests/test_tree_superrecord_benchmark.cpp (write_run_json), the g32 writer
in src/tests/tree_profile_json.h or the SUPERRECORD_* argument parsers is only
noticed hours into a CloudLab run, when summarize_cloudlab_profiles.py,
analyze_superrecord_sweep.py, run_entry_size_sweep.py, run_tree_t257_comparison.py
or run_treepir_backend_tuning.py rejects the file.  These tests spawn the binary
at 2^8 and 2^12 leaves with a fixed --trial-seed and feed its real output to
those readers.

Timings are nondeterministic, so nothing here asserts a millisecond value: the
assertions are on structure, field names, schedules, pins, exit codes and the
internal consistency between the aggregates and the raw samples they summarize.

The binary is $ONIONPIR_BENCH_BIN, else <repo>/build-x86_64-benchmark/Onion-PIR;
the module skips when it is absent and when the build is not the composite
first-dimension parameter point the generic benchmark is defined for.
"""

import json
import math
import os
import shutil
import statistics
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))

import analyze_superrecord_sweep
import run_entry_size_sweep
import run_tree_t257_comparison
import run_treepir_backend_tuning
import summarize_cloudlab_profiles

# The documented sweep shape: one h1 control and one bundled h3 row, no Pareto
# frontier rows, so a 2^8-leaf run stays near a second.
SMOKE_ENV = {
    "SUPERRECORD_SKIP_LEGACY": "1",
    "SUPERRECORD_BUNDLE_ARITIES": "1,3",
    "SUPERRECORD_MAX_ARITY": "3",
    "SUPERRECORD_MAX_FRONTIER": "0",
    "SUPERRECORD_H1_FRONTIER": "0",
}
# Stage columns are copied into stages_ms positionally against kStageLabels;
# summarize_cloudlab_profiles.py and benchmark_gadget_configs.py read them back
# by label, so the label set itself is part of the contract.
STAGE_LABELS = {
    "expand", "complete", "prepare", "scan", "finalize",
    "fold", "rotate", "project", "pack", "switch", "merge",
}

BINARY = None
WORK = None
_RUNS = {}


def _locate_binary():
    override = os.environ.get("ONIONPIR_BENCH_BIN")
    return Path(override) if override else REPO / "build-x86_64-benchmark" / "Onion-PIR"


def _clean_environment():
    """The knobs under test are environment variables the sweep scripts export;
    inherit everything else but start from a known state."""
    return {
        key: value
        for key, value in os.environ.items()
        if not key.startswith(("SUPERRECORD_", "CUCKOO_")) and key != "BENCHMARK_OUTPUT_JSON"
    }


def bench_run(name, argv, env_extra, output_key="SUPERRECORD_OUTPUT_JSON"):
    """Run the binary once per distinct invocation and cache the result."""
    if name not in _RUNS:
        env = _clean_environment()
        env.update(env_extra)
        out = WORK / f"{name}.json"
        if output_key is not None:
            env[output_key] = str(out)
        proc = subprocess.run([str(BINARY), *argv], env=env,
                              capture_output=True, text=True)
        _RUNS[name] = (proc, out)
    return _RUNS[name]


def bench_json(name, argv, env_extra, output_key="SUPERRECORD_OUTPUT_JSON"):
    proc, out = bench_run(name, argv, env_extra, output_key)
    if proc.returncode != 0:
        raise AssertionError(f"{name} exited {proc.returncode}:\n{proc.stdout}{proc.stderr}")
    return json.loads(out.read_text()), out


def nonfinite_paths(node, prefix=""):
    """predicted_server_ms defaults to NaN and is written with operator<<, which
    emits bare `nan`/`inf` tokens; report where any non-finite number sits."""
    bad = []
    if isinstance(node, dict):
        for key, value in node.items():
            bad += nonfinite_paths(value, f"{prefix}.{key}")
    elif isinstance(node, list):
        for index, value in enumerate(node):
            bad += nonfinite_paths(value, f"{prefix}[{index}]")
    elif isinstance(node, float) and not math.isfinite(node):
        bad.append(f"{prefix} = {node}")
    return bad


def setUpModule():
    global BINARY, WORK
    BINARY = _locate_binary()
    if not BINARY.is_file() or not os.access(BINARY, os.X_OK):
        raise unittest.SkipTest(f"benchmark binary not found: {BINARY}")
    WORK = Path(tempfile.mkdtemp(prefix="onionpir-json-contract."))
    # Cheapest generic-benchmark invocation (the planner keeps no candidate, so
    # it returns in ~0.02 s); it is also the CRIT-02 fixture below.  A build
    # without the composite first dimension exits 1 here with main.cpp's
    # "Error: ..." wrapper around the require_applicable reason.
    proc, _ = bench_run(
        "memory_capped",
        ["--test", "tree_superrecord_bench", "--leaf-count", "256",
         "--experiments", "2", "--warmup", "1", "--trial-seed", "7"],
        {**SMOKE_ENV, "SUPERRECORD_MEMORY_BYTES": "1000000"},
    )
    if proc.returncode != 0 and "composite first dimension" in proc.stdout + proc.stderr:
        shutil.rmtree(WORK, ignore_errors=True)
        raise unittest.SkipTest("build is not the composite first-dimension parameter point")


def tearDownModule():
    if WORK is not None:
        shutil.rmtree(WORK, ignore_errors=True)


class MeasuredRunContractTests(unittest.TestCase):
    """SRQR-02: the measured run the documented sweeps produce."""

    @classmethod
    def setUpClass(cls):
        cls.data, cls.path = bench_json(
            "measured",
            ["--test", "tree_superrecord_bench", "--leaf-count", "256",
             "--experiments", "3", "--warmup", "1", "--trial-seed", "7"],
            SMOKE_ENV,
        )

    def measured_candidates(self):
        return [c for c in self.data["candidates"] if c.get("eligible") and c.get("samples")]

    def test_run_header_records_the_requested_workload(self):
        self.assertEqual(self.data["schema_version"], "tree_superrecord_benchmark.v1")
        self.assertEqual(self.data["trial_seed"], 7)
        self.assertEqual(self.data["measured_trials"], 3)
        self.assertEqual(self.data["warmup_count"], 1)
        self.assertEqual(self.data["leaf_count"], 256)
        self.assertEqual(self.data["run_class"], "smoke_only")
        self.assertIs(self.data["plan_only"], False)
        self.assertEqual(self.data["response_packing"], "projection")

    def test_downstream_validators_accept_the_real_output(self):
        # The three readers the CloudLab pipeline runs on this file.
        self.assertEqual(
            summarize_cloudlab_profiles.validate_profile(self.data, self.path, 3, 256),
            "generic")
        run_entry_size_sweep.validate_bundle_arities(self.data, self.path)
        rows = analyze_superrecord_sweep.normalize_run(self.data)
        self.assertEqual({row["label"] for row in rows},
                         {"generic-h1-mandatory-0", "generic-h3-mandatory"})
        for row in rows:
            self.assertEqual(len(row["server_ms"]), 3, row["label"])

    def test_validators_reject_a_trial_count_that_disagrees(self):
        # Guards the test above from passing vacuously: the count contract is
        # what makes an interrupted CloudLab case fail instead of being summarized.
        with self.assertRaises(summarize_cloudlab_profiles.ProfileError):
            summarize_cloudlab_profiles.validate_profile(self.data, self.path, 16, 256)

    def test_every_candidate_measures_the_shared_leaf_schedule(self):
        schedule = self.data["leaf_schedule"]["measured"]
        self.assertEqual(len(schedule), 3)
        blocks = [b for b in self.data["block_execution_orders"] if not b["warmup"]]
        self.assertEqual([b["leaf"] for b in sorted(blocks, key=lambda b: b["block_id"])],
                         schedule)
        for candidate in self.measured_candidates():
            ordered = sorted(candidate["samples"], key=lambda s: s["block_id"])
            self.assertEqual([s["leaf"] for s in ordered], schedule, candidate["label"])

    def test_statistics_agree_with_the_raw_samples_they_summarize(self):
        for candidate in self.measured_candidates():
            ordered = sorted(candidate["samples"], key=lambda s: s["block_id"])
            series = {"server_total": [s["server_total_ms"] for s in ordered]}
            for stage in candidate["statistics"]["stages"]:
                series[stage] = [s["stages_ms"][stage] for s in ordered]
            self.assertEqual(set(series) - {"server_total"}, STAGE_LABELS,
                             candidate["label"])
            for name, raw in series.items():
                stats = (candidate["statistics"]["server_total"] if name == "server_total"
                         else candidate["statistics"]["stages"][name])
                where = f"{candidate['label']}.{name}"
                self.assertEqual(stats["raw_samples_ms"], raw, where)
                self.assert_close(stats["mean_ms"], statistics.fmean(raw), f"{where}.mean")
                self.assert_close(stats["median_ms"], statistics.median(raw), f"{where}.median")
                self.assert_close(stats["p95_ms"], sorted(raw)[math.ceil(0.95 * len(raw)) - 1],
                                  f"{where}.p95")
                self.assert_close(stats["stddev_ms"], statistics.stdev(raw), f"{where}.stddev")

    def assert_close(self, actual, expected, where):
        self.assertTrue(math.isclose(actual, expected, rel_tol=1e-9, abs_tol=1e-12),
                        f"{where}: reported {actual}, samples give {expected}")

    def test_wire_and_memory_totals_match_the_shape_they_describe(self):
        identity = self.data["scheme_identity"]
        for candidate in self.measured_candidates():
            where = candidate["label"]
            # Serialized small-q payload: two polynomials per response ciphertext.
            expected_bytes = (candidate["shape"]["response_ciphertexts"] * 2
                              * identity["ring_degree"] * identity["response_modulus_bits"] // 8)
            self.assertEqual(candidate["wire"]["actual_response_ciphertext_payload_bytes"],
                             expected_bytes, where)
            for sample in candidate["samples"]:
                self.assertEqual(sample["response_payload_bytes"], expected_bytes, where)
            self.assertEqual(candidate["memory"]["database_bytes"],
                             candidate["candidate_metrics"]["optimized_bytes"], where)
            self.assertIs(candidate["key_coverage"]["matches"], True, where)

    def test_projection_packing_leaves_the_merge_columns_empty(self):
        for candidate in self.measured_candidates():
            where = candidate["label"]
            metadata = candidate["response_packing_metadata"]
            self.assertEqual(metadata["mode"], "projection", where)
            self.assertEqual(metadata["actual_mlwe_merge_keyswitch_count"], 0, where)
            for sample in candidate["samples"]:
                self.assertEqual(sample["stages_ms"]["merge"], 0, where)
                self.assertGreater(sample["stages_ms"]["project"], 0, where)
                self.assertEqual(sample["response_merge_keyswitches"], 0, where)


class ResponsePackingTests(unittest.TestCase):
    """SRQR-03: SUPERRECORD_RESPONSE_PACKING=mlwe, the knob benchmark_gadget_configs.py
    sets per label, switches the served path and the stage columns with it."""

    @classmethod
    def setUpClass(cls):
        cls.data, cls.path = bench_json(
            "mlwe",
            ["--test", "tree_superrecord_bench", "--leaf-count", "256",
             "--experiments", "3", "--warmup", "1", "--trial-seed", "7"],
            {**SMOKE_ENV, "SUPERRECORD_BUNDLE_ARITIES": "3",
             "SUPERRECORD_RESPONSE_PACKING": "mlwe"},
        )
        cls.candidate = next(c for c in cls.data["candidates"] if c.get("samples"))

    def test_the_knob_reaches_the_run_the_metadata_and_the_noise_gate(self):
        self.assertEqual(self.data["response_packing"], "mlwe")
        self.assertEqual(self.candidate["response_packing_metadata"]["mode"], "mlwe")
        self.assertEqual(self.candidate["target_noise_result"]["response_packing"], "mlwe")

    def test_merge_replaces_the_projection_stage_columns(self):
        for sample in self.candidate["samples"]:
            self.assertEqual(set(sample["stages_ms"]), STAGE_LABELS)
            self.assertGreater(sample["stages_ms"]["merge"], 0)
            self.assertEqual(sample["stages_ms"]["project"], 0)
            self.assertEqual(sample["stages_ms"]["pack"], 0)

    def test_measured_key_switches_match_the_planned_merge_metadata(self):
        metadata = self.candidate["response_packing_metadata"]
        shape = self.candidate["shape"]
        self.assertGreater(metadata["actual_mlwe_merge_keyswitch_count"], 0)
        for sample in self.candidate["samples"]:
            self.assertEqual(sample["response_merge_keyswitches"],
                             metadata["actual_mlwe_merge_keyswitch_count"])
        # One merge buffer slot per merged path level, two polynomials of
        # 64-bit coefficients each (commit 0ac2393 shares these across fragments).
        merged_slots = min(shape["path_levels"], shape["records_per_plaintext"])
        self.assertEqual(metadata["response_merge_buffer_bytes"],
                         merged_slots * 2 * self.data["scheme_identity"]["ring_degree"] * 8)


class LegacyControlRowTests(unittest.TestCase):
    """CRIT-01: with SUPERRECORD_SKIP_LEGACY unset - the state every documented
    local sweep runs in - the run carries a measured legacy-g32 control row and
    the analyzer pairs the generic rows against it."""

    @classmethod
    def setUpClass(cls):
        env = {k: v for k, v in SMOKE_ENV.items() if k != "SUPERRECORD_SKIP_LEGACY"}
        cls.data, cls.path = bench_json(
            "legacy_control",
            ["--test", "tree_superrecord_bench", "--leaf-count", "256",
             "--experiments", "2", "--warmup", "1", "--trial-seed", "7"],
            env,
        )
        cls.stdout = _RUNS["legacy_control"][0].stdout

    def test_the_control_row_is_measured_and_shaped_like_the_generic_rows(self):
        controls = [c for c in self.data["candidates"] if c["label"] == "legacy-g32"]
        self.assertEqual(len(controls), 1)
        control = controls[0]
        self.assertIs(control["eligible"], True)
        self.assertIs(control["mandatory_control"], True)
        self.assertIs(control["is_generic"], False)
        self.assertEqual(len(control["samples"]), 2)
        for sample in control["samples"]:
            self.assertGreater(sample["server_total_ms"], 0)
            self.assertEqual(set(sample["stages_ms"]), STAGE_LABELS)
        # The legacy row is built by a separate RunResult path; a field it
        # forgets to fill would only show up here.
        generic = next(c for c in self.data["candidates"] if c["is_generic"])
        self.assertEqual(set(control), set(generic))

    def test_the_analysis_chain_treats_it_as_the_legacy_control(self):
        self.assertEqual(
            summarize_cloudlab_profiles.validate_profile(self.data, self.path, 2, 256),
            "generic")
        rows = analyze_superrecord_sweep.normalize_run(self.data)
        legacy = [row for row in rows if row["legacy"]]
        self.assertEqual([row["label"] for row in legacy], ["legacy-g32"])
        result = analyze_superrecord_sweep.analyze_run(self.data, seed=1)
        generic_labels = [row["label"] for row in rows if not row["legacy"]]
        self.assertEqual(sorted(result["comparisons"]), sorted(generic_labels))
        for label in generic_labels:
            self.assertIn("legacy-g32", result["comparisons"][label])
        self.assertIn("speedup vs legacy", self.stdout)


class MemoryCappedRunTests(unittest.TestCase):
    """CRIT-02: an over-tight SUPERRECORD_MEMORY_BYTES (a knob four scripts set)
    leaves the planner with nothing, and the Python validator is the only thing
    between the empty file it still writes and a silent empty CloudLab profile."""

    @classmethod
    def setUpClass(cls):
        cls.proc, cls.path = _RUNS["memory_capped"]
        cls.data = json.loads(cls.path.read_text())

    def test_the_writer_emits_a_valid_but_empty_run(self):
        self.assertEqual(self.data["candidates"], [])
        self.assertEqual(self.data["planner"]["candidate_count"], 0)
        self.assertEqual(self.data["planner"]["max_steady_state_bytes"], 1000000)
        self.assertTrue(all(block["candidate_indices"] == []
                            for block in self.data["block_execution_orders"]))

    def test_the_validator_rejects_it_and_the_analyzer_yields_no_rows(self):
        with self.assertRaises(summarize_cloudlab_profiles.ProfileError) as caught:
            summarize_cloudlab_profiles.validate_profile(self.data, self.path, 2, 256)
        self.assertIn("missing measured h1 candidate", str(caught.exception))
        self.assertEqual(analyze_superrecord_sweep.normalize_run(self.data), [])


class PlanOnlyRunTests(unittest.TestCase):
    """SRQR-07: the plan-only file run_superrecord_sweep.py --plan-only and
    run_treepir_backend_tuning.py parse."""

    @classmethod
    def setUpClass(cls):
        cls.data, cls.path = bench_json(
            "plan_only",
            ["--test", "tree_superrecord_bench", "--leaf-count", "256"],
            {"SUPERRECORD_PLAN_ONLY": "1", "SUPERRECORD_SKIP_LEGACY": "1"},
        )

    def test_the_plan_is_marked_and_carries_no_measurements(self):
        self.assertIs(self.data["plan_only"], True)
        self.assertEqual(self.data["run_class"], "plan_only")
        self.assertEqual(self.data["block_execution_orders"], [])
        self.assertEqual(sorted(self.data["candidate_execution_order"]),
                         list(range(len(self.data["candidates"]))))
        for candidate in self.data["candidates"]:
            self.assertIs(candidate["eligible"], False, candidate["label"])
            self.assertEqual(candidate["samples"], [], candidate["label"])
            self.assertIsNone(candidate["ineligible_reason"], candidate["label"])

    def test_every_number_in_the_plan_is_finite(self):
        # predicted_server_ms starts as NaN; any row written without ranking
        # would make the file unparseable for the CloudLab scheduler.
        self.assertEqual(nonfinite_paths(self.data), [])
        for candidate in self.data["candidates"]:
            metrics = candidate["candidate_metrics"]
            self.assertGreater(metrics["predicted_server_ms"], 0, candidate["label"])
            self.assertGreater(metrics["optimized_steady_state_bytes_estimate"], 0,
                               candidate["label"])
        self.assertEqual(analyze_superrecord_sweep.normalize_run(self.data), [])


class EnvironmentContractTests(unittest.TestCase):
    """SRQR-05: the SUPERRECORD_* argument contract benchmark_gadget_configs.py
    and benchmark_alpha_ntt_ab.py depend on."""

    PIN = "3,64,1,2,1"  # h, G, P, N0, b - N0 != b, so a swapped matcher shows up

    def test_a_pin_selects_exactly_the_shape_it_names(self):
        data, _ = bench_json(
            "pinned",
            ["--test", "tree_superrecord_bench", "--leaf-count", "256"],
            {"SUPERRECORD_PLAN_ONLY": "1", "SUPERRECORD_SKIP_LEGACY": "1",
             "SUPERRECORD_PIN_SHAPE": self.PIN},
        )
        self.assertEqual(data["benchmark_shape_pin"], self.PIN)
        self.assertGreater(data["planner"]["candidate_count"], 0)
        self.assertEqual(data["planner"]["h1_candidate_count"], 0)
        self.assertTrue(data["candidates"])
        for candidate in data["candidates"]:
            shape = candidate["shape"]
            self.assertEqual(
                [shape["bundle_arity"], shape["record_coeffs"], shape["plane_count"],
                 shape["N0"], shape["beta_bits"]],
                [int(part) for part in self.PIN.split(",")], candidate["label"])
            self.assertEqual(candidate["samples"], [])

    def test_malformed_and_infeasible_settings_are_rejected(self):
        cases = {
            "pin_infeasible": ({"SUPERRECORD_PIN_SHAPE": "3,64,1,999,0"},
                               "pinned benchmark shape is not feasible"),
            "pin_short": ({"SUPERRECORD_PIN_SHAPE": "3,64,1"},
                          "SUPERRECORD_PIN_SHAPE must be h,G,P,N0,b"),
            "packing_bad": ({"SUPERRECORD_RESPONSE_PACKING": "foo"},
                            "SUPERRECORD_RESPONSE_PACKING must be projection or mlwe"),
            "key_mode_bad": ({"SUPERRECORD_KEY_MODE": "nope"},
                             "SUPERRECORD_KEY_MODE must be fixed or candidate-specific"),
            "arity_bad": ({"SUPERRECORD_BUNDLE_ARITIES": "9", "SUPERRECORD_MAX_ARITY": "8"},
                          "SUPERRECORD_BUNDLE_ARITIES must be within the bundle arity policy"),
            "max_responses_bad": ({"SUPERRECORD_MAX_RESPONSES": "two"},
                                  "SUPERRECORD_MAX_RESPONSES must be a non-negative size_t integer"),
        }
        for name, (env, message) in cases.items():
            with self.subTest(case=name):
                proc, out = bench_run(
                    name,
                    ["--test", "tree_superrecord_bench", "--leaf-count", "256"],
                    {"SUPERRECORD_PLAN_ONLY": "1", "SUPERRECORD_SKIP_LEGACY": "1", **env},
                )
                self.assertNotEqual(proc.returncode, 0)
                self.assertIn(message, proc.stdout + proc.stderr)
                self.assertFalse(out.exists(), "a rejected run must not write JSON")


class PairedScheduleTests(unittest.TestCase):
    """PR-05: the g32 baseline JSON run_tree_t257_comparison.py validates, the
    plan-only leaf schedule run_treepir_backend_tuning.py reads, and the fact
    that both benchmarks measure the same leaves - which is what makes the
    cross-scheme comparisons paired."""

    LEAVES = 4096

    @classmethod
    def setUpClass(cls):
        cls.g32, cls.g32_path = bench_json(
            "g32",
            ["--test", "tree_bench_g32", "--leaf-count", str(cls.LEAVES),
             "--experiments", "2", "--warmup", "1", "--trial-seed", "7"],
            {}, output_key="BENCHMARK_OUTPUT_JSON")
        cls.plan, cls.plan_path = bench_json(
            "plan_paired",
            ["--test", "tree_superrecord_bench", "--leaf-count", str(cls.LEAVES),
             "--experiments", "2", "--warmup", "1", "--trial-seed", "7"],
            {"SUPERRECORD_PLAN_ONLY": "1", "SUPERRECORD_SKIP_LEGACY": "1",
             "SUPERRECORD_BUNDLE_ARITIES": "1,3", "SUPERRECORD_MAX_ARITY": "3"})

    def test_the_g32_baseline_passes_the_t257_comparison_validator(self):
        run_tree_t257_comparison.validate_result(self.g32, self.g32_path, 4099,
                                                self.LEAVES, 2, 1)
        self.assertLessEqual(
            {"modeled_query_bytes_per_trial", "actual_response_bytes_per_trial",
             "modeled_helper_key_bytes"},
            set(self.g32["communication"]))
        self.assertIn("min_noise_budget_bits", self.g32["metadata"])

    def test_the_tuning_script_accepts_the_plan_only_leaf_schedule(self):
        leaves = run_treepir_backend_tuning.validate_leaf_schedule(
            self.plan_path, trials=2, warmups=1, leaf_count=self.LEAVES)
        self.assertEqual(len(leaves), 3)

    def test_both_benchmarks_measure_the_same_leaves_for_one_seed(self):
        self.assertEqual([sample["leaf"] for sample in self.g32["samples"]],
                         self.plan["leaf_schedule"]["measured"])


if __name__ == "__main__":
    unittest.main()
