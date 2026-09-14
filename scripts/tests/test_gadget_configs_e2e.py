#!/usr/bin/env python3
"""scripts/benchmark_gadget_configs.py driven end to end against the real binary.

test_gadget_configs.py replaces run_measured with a fake that writes a
hand-written profile, so nothing checks that the driver's SUPERRECORD_* strings
are the ones the C++ parsers accept: a pin the planner cannot satisfy is only
reported after all four rounds have run, which is the failure that wastes a
cluster session.  These two tests run the documented A/B (the same binary under
two labels) at 2^12 leaves and assert the contract both sides agree on - the
pinned layout, the gadget/key metadata the report prints, the ABBA run order,
and the mlwe packing path whose merge key-switch accounting changed in 0ac2393.

Timings are nondeterministic; the assertions are on fields, counts and order.

The binary is $ONIONPIR_BENCH_BIN, else <repo>/build-x86_64-benchmark/Onion-PIR.
"""

import contextlib
import io
import json
import os
import shutil
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path
from unittest.mock import patch

REPO = Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))

import benchmark_gadget_configs as gadget

# The planner's mandatory h3 shape at 2^12 leaves under this driver's policy.
SHAPE = "3,128,1,16,3"
EXPANSION_HEIGHT = "7"
LEAF_COUNT = "4096"
EXPERIMENTS = "2"
GADGET_IDENTITY_KEYS = {
    "selector_gadget_rows", "selector_base_log2", "selector_low_bits",
    "completion_key_gadget_rows", "completion_key_base_log2",
    "bv_ks_rows", "bv_ks_base_log2", "bv_ks_low_bits",
}

BINARY = None


def setUpModule():
    global BINARY
    override = os.environ.get("ONIONPIR_BENCH_BIN")
    BINARY = Path(override) if override else REPO / "build-x86_64-benchmark" / "Onion-PIR"
    if not BINARY.is_file() or not os.access(BINARY, os.X_OK):
        raise unittest.SkipTest(f"benchmark binary not found: {BINARY}")
    # Cheapest generic-benchmark invocation: the memory cap leaves the planner
    # no candidate, so it returns in ~0.02 s.  A build without the composite
    # first dimension instead exits 1 with the require_applicable reason.
    clean = {k: v for k, v in os.environ.items() if not k.startswith("SUPERRECORD_")}
    probe = subprocess.run(
        [str(BINARY), "--test", "tree_superrecord_bench", "--leaf-count", "256"],
        env={**clean, "SUPERRECORD_MEMORY_BYTES": "1000000",
             "SUPERRECORD_SKIP_LEGACY": "1", "SUPERRECORD_PLAN_ONLY": "1"},
        capture_output=True, text=True)
    if probe.returncode != 0 and "composite first dimension" in probe.stdout + probe.stderr:
        raise unittest.SkipTest("build is not the composite first-dimension parameter point")


def run_driver(out_dir, *extra):
    """Invoke the driver the way the A/B runbook does and return its artifacts."""
    argv = [
        "benchmark_gadget_configs.py",
        "--binary", f"a={BINARY}",
        "--binary", f"b={BINARY}",
        "--output", str(out_dir),
        "--experiments", EXPERIMENTS,
        "--leaf-count", LEAF_COUNT,
        "--shape", SHAPE,
        "--expansion-height", EXPANSION_HEIGHT,
        "--memory-gib", "2",
        *extra,
    ]
    # The driver copies os.environ into every child; start from a state where no
    # SUPERRECORD_ knob is inherited so the run is the driver's own.
    clean = {k: v for k, v in os.environ.items() if not k.startswith("SUPERRECORD_")}
    with patch.dict(os.environ, clean, clear=True), patch.object(sys, "argv", argv):
        with contextlib.redirect_stdout(io.StringIO()):
            gadget.main()
    return (json.loads((out_dir / "comparison.json").read_text()),
            json.loads((out_dir / "manifest.json").read_text()))


class GadgetAbDriverTests(unittest.TestCase):
    """The two-round A/B of one binary under two labels."""

    @classmethod
    def setUpClass(cls):
        cls.out = Path(tempfile.mkdtemp(prefix="onionpir-gadget-ab."))
        cls.addClassCleanup(shutil.rmtree, cls.out, ignore_errors=True)
        cls.comparison, cls.manifest = run_driver(cls.out, "--rounds", "2")

    def test_two_rounds_of_the_same_binary_are_accepted_as_comparable(self):
        self.assertEqual(self.comparison["state"], "complete")
        self.assertEqual(self.comparison["completed_labels"], ["a", "b"])
        self.assertEqual(self.comparison["rejected_labels"], {})
        # The pin the driver builds is the layout the binary actually measured.
        self.assertEqual(self.comparison["layout"], {
            "entry_bits": 256, "leaf_count": 4096, "bundle_arity": 3,
            "record_coeffs": 128, "plane_count": 1, "records_per_plaintext": 16,
            "N0": 16, "beta_bits": 3, "expansion_height": 7})
        self.assertEqual([run["name"] for run in self.manifest["runs"]],
                         ["round0-a", "round0-b", "round1-b", "round1-a"])
        for run in self.manifest["runs"]:
            self.assertEqual(run["state"], "complete", run["name"])
            self.assertEqual(run["environment"]["SUPERRECORD_PIN_SHAPE"], SHAPE)
            self.assertEqual(run["environment"]["SUPERRECORD_EXPANSION_HEIGHTS"],
                             EXPANSION_HEIGHT)

    def test_the_summary_carries_the_gadget_and_key_metadata_it_reports(self):
        for label in ("a", "b"):
            summary = self.comparison["summary"][label]
            # Two rounds of two experiments, merged into one candidate.
            self.assertEqual(summary["completed_samples"], 4, label)
            metadata = summary["gadget_metadata"]
            self.assertEqual(set(metadata["gadget_identity"]), GADGET_IDENTITY_KEYS, label)
            self.assertEqual(summary["key_metadata"],
                             {"required_key_height": 7, "registered_key_height": 11,
                              "scheme_registered_session_key_height": 11}, label)
            self.assertEqual(summary["packing"], "projection", label)
            self.assertEqual(summary["counter_metadata"]["response_merge_keyswitches_mean"],
                             0, label)
            self.assertEqual(summary["stages_ms_mean"]["merge"], 0, label)


class ResponsePackingFlagTests(unittest.TestCase):
    """--packing b=mlwe on one side only, the comparison the gadget runbook makes."""

    @classmethod
    def setUpClass(cls):
        cls.out = Path(tempfile.mkdtemp(prefix="onionpir-gadget-packing."))
        cls.addClassCleanup(shutil.rmtree, cls.out, ignore_errors=True)
        cls.comparison, _ = run_driver(cls.out, "--rounds", "1", "--packing", "b=mlwe")

    def test_the_packing_flag_switches_only_the_labelled_side(self):
        comparison = self.comparison
        self.assertEqual(comparison["state"], "complete")
        projection, mlwe = comparison["summary"]["a"], comparison["summary"]["b"]
        self.assertEqual(projection["packing"], "projection")
        self.assertEqual(mlwe["packing"], "mlwe")
        self.assertGreater(mlwe["counter_metadata"]["response_merge_keyswitches_mean"], 0)
        self.assertGreater(mlwe["stages_ms_mean"]["merge"], 0)
        self.assertEqual(projection["stages_ms_mean"]["merge"], 0)
        self.assertEqual(projection["counter_metadata"]["response_merge_keyswitches_mean"], 0)
        # The knob has to reach the served path, not just the driver's label.
        raw = json.loads((self.out / "round0-b.json").read_text())
        self.assertEqual(raw["response_packing"], "mlwe")
        measured = [c for c in raw["candidates"] if c.get("samples")]
        self.assertEqual([c["target_noise_result"]["response_packing"] for c in measured],
                         ["mlwe"])


if __name__ == "__main__":
    unittest.main()
