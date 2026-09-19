#!/usr/bin/env python3
import json
import subprocess
import tempfile
import unittest
from pathlib import Path


ROOT = Path(__file__).resolve().parents[2]
import sys
sys.path.insert(0, str(ROOT / "scripts"))
from path_baselines.report import validate_result

WRAPPER = ROOT / "scripts" / "path_baselines" / "respire" / "respire_path_workload.py"


class RespirePathAdapterDryRunTest(unittest.TestCase):
    def dry_run(self, height: int, leaves: list[int], warmup: int, trials: int) -> dict:
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            leaf_file = root / "leaves.txt"
            leaf_file.write_text("\n".join(map(str, leaves)) + "\n", encoding="utf-8")
            out = root / "out.json"
            subprocess.run(
                [
                    "python3",
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
                    "--dry-run",
                ],
                cwd=ROOT,
                text=True,
                capture_output=True,
                check=True,
            )
            return json.loads(out.read_text(encoding="utf-8"))

    def test_h24_path_schema_and_distinct_padding(self):
        data = self.dry_run(24, [0, 7], warmup=1, trials=1)
        self.assertEqual(data["schema_version"], "path-baseline-v1")
        self.assertEqual(data["scheme"], "respire")
        self.assertEqual(data["path_mode"], "ancestor_root_included")
        sample = data["dry_run_samples"][1]
        self.assertEqual(sample["path_indices"][0], 0)
        self.assertEqual(sample["path_indices"][-1], (1 << 24) - 1 + 7)
        self.assertEqual(len(sample["path_indices"]), 25)
        self.assertEqual(len(sample["request_indices"]), 32)
        self.assertEqual(len(set(sample["request_indices"])), 32)
        self.assertEqual(sample["request_indices"][:25], sample["path_indices"])
        self.assertIn((1 << 25) - 1, sample["request_indices"][25:])
        self.assertEqual(data["logical_records"], (1 << 25) - 1)
        self.assertEqual(data["backend_records"], 1 << 25)

    def test_shared_golden_hex_values_are_used(self):
        data = self.dry_run(8, [192], warmup=0, trials=1)
        sample = data["dry_run_samples"][0]
        self.assertEqual(sample["expected_path_hex"][0], "b8782335a8e76c4bf81ce235c978f777b654f924299739320bda3618dcc37790")
        self.assertEqual(sample["path_indices"][3], (1 << 3) - 1 + 6)
        self.assertEqual(sample["expected_path_hex"][3], "3849527f67cb7a2c02bb1678b692eb093d424f3e552d646e20c3159427402066")

    def test_rejects_zero_trials_and_short_leaf_file(self):
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            leaf_file = root / "leaves.txt"
            leaf_file.write_text("0\n", encoding="utf-8")
            zero = subprocess.run(
                [
                    "python3",
                    str(WRAPPER),
                    "--height",
                    "8",
                    "--leaf-file",
                    str(leaf_file),
                    "--warmup",
                    "0",
                    "--trials",
                    "0",
                    "--dry-run",
                ],
                cwd=ROOT,
                text=True,
                capture_output=True,
            )
            self.assertNotEqual(zero.returncode, 0)
            self.assertIn("--trials must be greater than zero", zero.stderr)

            short = subprocess.run(
                [
                    "python3",
                    str(WRAPPER),
                    "--height",
                    "8",
                    "--leaf-file",
                    str(leaf_file),
                    "--warmup",
                    "1",
                    "--trials",
                    "1",
                    "--dry-run",
                ],
                cwd=ROOT,
                text=True,
                capture_output=True,
            )
            self.assertNotEqual(short.returncode, 0)
            self.assertIn("requested warmup+trials=2", short.stderr)

    def test_real_h8_encrypted_output_passes_shared_validator(self):
        leaves = [0, 192, 7]
        with tempfile.TemporaryDirectory() as td:
            root = Path(td)
            leaf_file = root / "leaves.txt"
            leaf_file.write_text("\n".join(map(str, leaves)) + "\n", encoding="utf-8")
            out = root / "respire-h8.json"
            subprocess.run(
                [
                    "python3",
                    str(WRAPPER),
                    "--height",
                    "8",
                    "--leaf-file",
                    str(leaf_file),
                    "--warmup",
                    "1",
                    "--trials",
                    "2",
                    "--output",
                    str(out),
                ],
                cwd=ROOT,
                text=True,
                capture_output=True,
                check=True,
            )
            data = json.loads(out.read_text(encoding="utf-8"))
        validate_result(data, height=8, leaves=leaves, warmups=1, trials=2)
        self.assertEqual(data["samples"][0]["correct_entries"], 9)
        self.assertEqual(data["samples"][0]["verified_dummy_entries"], 23)
        self.assertEqual(data["samples"][0]["total_verified_entries"], 32)


if __name__ == "__main__":
    unittest.main()
