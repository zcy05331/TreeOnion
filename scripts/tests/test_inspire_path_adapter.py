#!/usr/bin/env python3
import json
import os
import subprocess
import sys
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[2]
WRAPPER = ROOT / "scripts" / "path_baselines" / "inspire" / "inspire_path_adapter.py"
POULPY_DUMPS = {
    "querying_keys.bin",
    "conversion_keys.bin",
    "automorphism_left.bin",
    "automorphism_right.bin",
    "query.bin",
    "response.bin",
}


class InspirePathAdapterSmoke(unittest.TestCase):
    def run_adapter(self, height: int, leaves, warmup: int, trials: int):
        with tempfile.TemporaryDirectory() as td:
            td = Path(td)
            leaf_file = td / "leaves.txt"
            leaf_file.write_text("\n".join(str(x) for x in leaves), encoding="utf-8")
            out = td / "report.json"
            env = os.environ.copy()
            env.setdefault("PIR_THREADS", "1")
            env.setdefault("RUSTC_BOOTSTRAP", "1")
            env.setdefault("CARGO_TARGET_DIR", str(ROOT / ".benchmarks" / "inspire-path" / "target"))
            subprocess.run(
                [
                    sys.executable,
                    str(WRAPPER),
                    "--height",
                    str(height),
                    "--leaf-file",
                    str(leaf_file),
                    "--warmup",
                    str(warmup),
                    "--trials",
                    str(trials),
                    "--output",
                    str(out),
                ],
                cwd=ROOT,
                env=env,
                text=True,
                capture_output=True,
                check=True,
            )
            return json.loads(out.read_text(encoding="utf-8"))

    def test_h1_real_crypto_full_path_includes_root(self):
        report = self.run_adapter(1, [0, 1], warmup=1, trials=1)
        self.assertEqual(report["schema_version"], "path-baseline-v1")
        self.assertEqual(report["scheme"], "inspire_layerwise")
        self.assertEqual(report["path_mode"], "ancestor_root_included")
        self.assertEqual(report["model"], "Layerwise-InsPIRe-full-path")
        self.assertEqual(report["implementation_provenance"]["implementation"], "third_party_poulpy_pir")
        self.assertFalse(report["implementation_provenance"]["official_author_code"])
        self.assertEqual(report["run_mode"], "real_crypto_decrypt")
        self.assertTrue(report["root_included"])
        self.assertEqual(report["layout"]["per_level_databases"], 2)
        self.assertEqual(report["logical_records"], (1 << 2) - 1)
        self.assertEqual(report["record_bytes"], 32)
        self.assertEqual(report["communication"]["query"], "actual_serialized")
        self.assertEqual(report["communication"]["response"], "actual_serialized")
        self.assertEqual(len(report["warmup_samples"]), 1)
        self.assertEqual(len(report["samples"]), 1)
        self.assertTrue(report["measured_summary"]["all_correct"])
        self.assertGreater(report["measured_summary"]["wire_query_bytes_total_mean"], 0)
        self.assertGreater(report["measured_summary"]["wire_response_bytes_total_mean"], 0)
        measured = [r for r in report["results"] if r["phase"] == "measured"]
        self.assertEqual(len(measured), 1)
        path = measured[0]["path_nodes"]
        self.assertEqual(len(path), 2)
        self.assertEqual(path[0]["level"], 0)
        self.assertEqual(path[0]["flat_bfs_id"], 0)
        self.assertEqual(path[-1]["level"], 1)
        self.assertEqual(path[-1]["node_index"], 1)
        self.assertTrue(all(node["correct"] for node in path))

    def test_h1_sums_level_wire(self):
        with tempfile.TemporaryDirectory() as td:
            td = Path(td)
            leaf_file = td / "leaves.txt"
            leaf_file.write_text("0\n", encoding="utf-8")
            out = td / "report.json"
            env = os.environ.copy()
            env.setdefault("PIR_THREADS", "1")
            env.setdefault("RUSTC_BOOTSTRAP", "1")
            env.setdefault("CARGO_TARGET_DIR", str(ROOT / ".benchmarks" / "inspire-path" / "target"))
            subprocess.run(
                [
                    sys.executable,
                    str(WRAPPER),
                    "--height",
                    "1",
                    "--leaf-file",
                    str(leaf_file),
                    "--warmup",
                    "0",
                    "--trials",
                    "1",
                    "--output",
                    str(out),
                ],
                cwd=ROOT,
                env=env,
                text=True,
                capture_output=True,
                check=True,
            )
            report = json.loads(out.read_text(encoding="utf-8"))
        result = report["results"][0]
        self.assertEqual(len(result["path_nodes"]), 2)
        self.assertEqual(result["wire_query_bytes"], sum(n["wire_query_bytes"] for n in result["path_nodes"]))
        self.assertEqual(result["wire_response_bytes"], sum(n["wire_response_bytes"] for n in result["path_nodes"]))
        self.assertEqual(report["samples"][0]["query_bytes"], result["wire_query_bytes"])
        self.assertEqual(report["samples"][0]["response_bytes"], result["wire_response_bytes"])
        self.assertTrue(result["correct"])
        self.assertIn("offline_ms", report["stage_definitions"])

    def test_h8_real_crypto_smoke(self):
        report = self.run_adapter(8, [0, 17, 123], warmup=1, trials=2)
        self.assertTrue(report["measured_summary"]["all_correct"])
        self.assertEqual(len(report["samples"]), 2)
        self.assertEqual(report["samples"][0]["path_entries"], 9)

    def test_rejects_out_of_range_leaf(self):
        with tempfile.TemporaryDirectory() as td:
            td = Path(td)
            leaf_file = td / "leaves.txt"
            leaf_file.write_text("256\n", encoding="utf-8")
            out = td / "report.json"
            proc = subprocess.run(
                [
                    sys.executable,
                    str(WRAPPER),
                    "--height",
                    "8",
                    "--leaf-file",
                    str(leaf_file),
                    "--warmup",
                    "0",
                    "--trials",
                    "1",
                    "--output",
                    str(out),
                ],
                cwd=ROOT,
                env={
                    **os.environ,
                    "RUSTC_BOOTSTRAP": "1",
                    "CARGO_TARGET_DIR": str(ROOT / ".benchmarks" / "inspire-path" / "target"),
                },
                text=True,
                capture_output=True,
            )
            self.assertNotEqual(proc.returncode, 0)
            self.assertIn("out of range", proc.stderr)

    def test_wrapper_keeps_poulpy_artifacts_out_of_repo_root(self):
        before = {name for name in POULPY_DUMPS if (ROOT / name).exists()}
        self.run_adapter(1, [0], warmup=0, trials=1)
        after = {name for name in POULPY_DUMPS if (ROOT / name).exists()}
        self.assertEqual(after, before)


if __name__ == "__main__":
    unittest.main()
