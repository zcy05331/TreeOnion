#!/usr/bin/env python3
import json
import re
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

    def test_h22_h23_dry_run_use_small_formal_geometries(self):
        for height in (22, 23):
            with self.subTest(height=height):
                data = self.dry_run(height, [0, (1 << height) - 1], warmup=1, trials=1)
                sample = data["dry_run_samples"][1]
                self.assertEqual(data["height"], height)
                self.assertEqual(data["leaf_count"], 1 << height)
                self.assertEqual(data["logical_records"], (1 << (height + 1)) - 1)
                self.assertEqual(data["backend_records"], 1 << (height + 1))
                self.assertEqual(len(sample["path_indices"]), height + 1)
                self.assertEqual(len(sample["request_indices"]), 32)
                self.assertEqual(sample["path_indices"][-1], (1 << (height + 1)) - 2)

    def test_h26_dry_run_uses_4gib_geometry(self):
        data = self.dry_run(26, [0, (1 << 26) - 1], warmup=1, trials=1)
        sample = data["dry_run_samples"][1]
        self.assertEqual(data["height"], 26)
        self.assertEqual(data["leaf_count"], 1 << 26)
        self.assertEqual(data["logical_records"], (1 << 27) - 1)
        self.assertEqual(data["backend_records"], 1 << 27)
        self.assertEqual(len(sample["path_indices"]), 27)
        self.assertEqual(len(sample["request_indices"]), 32)
        self.assertEqual(sample["path_indices"][-1], (1 << 27) - 2)

    def test_rust_dispatch_includes_distinct_formal_shapes(self):
        main_rs = (ROOT / "scripts/path_baselines/respire/path_workload/main.rs").read_text(encoding="utf-8")
        params_rs = (ROOT / "scripts/path_baselines/respire/path_workload/params.rs").read_text(encoding="utf-8")
        report_rs = (ROOT / "scripts/path_baselines/respire/path_workload/report.rs").read_text(encoding="utf-8")
        self.assertIn("22 => runner::execute::<params::FormalH22>", main_rs)
        self.assertIn("23 => runner::execute::<params::FormalH23>", main_rs)
        self.assertIn("26 => runner::execute::<params::FormalH26>", main_rs)
        self.assertIn("pub type FormalH22 = CuckooRespireImpl<32, 49, { 1usize << 23 }, FormalH22Base>;", params_rs)
        self.assertIn("pub type FormalH23 = CuckooRespireImpl<32, 49, { 1usize << 24 }, FormalH23Base>;", params_rs)
        self.assertIn("pub type FormalH26 = CuckooRespireImpl<32, 49, { 1usize << 27 }, FormalH26Base>;", params_rs)
        self.assertIn("FactoryParams::batch_256(49, 8, 7, 7)", params_rs)
        self.assertIn("FactoryParams::batch_256(49, 8, 8, 7)", params_rs)
        self.assertIn("FactoryParams::batch_256(49, 8, 9, 9)", params_rs)
        self.assertIn('label: "formal_h22_256mib_batch_256"', params_rs)
        self.assertIn('label: "formal_h23_512mib_batch_256"', params_rs)
        self.assertIn('label: "formal_h26_4gib_batch_256"', params_rs)
        h22 = re.search(r"FORMAL_H22_SHAPE:.*?nu1: (\d+),\s+nu2: (\d+),", params_rs, re.S)
        h23 = re.search(r"FORMAL_H23_SHAPE:.*?nu1: (\d+),\s+nu2: (\d+),", params_rs, re.S)
        h24 = re.search(r"FORMAL_H24_SHAPE:.*?nu1: (\d+),\s+nu2: (\d+),", params_rs, re.S)
        h26 = re.search(r"FORMAL_H26_SHAPE:.*?nu1: (\d+),\s+nu2: (\d+),", params_rs, re.S)
        self.assertIsNotNone(h22)
        self.assertIsNotNone(h23)
        self.assertIsNotNone(h24)
        self.assertIsNotNone(h26)
        self.assertEqual(h22.groups(), ("7", "7"))
        self.assertEqual(h23.groups(), ("8", "7"))
        self.assertNotEqual(h24.groups(), h26.groups())
        h24_nu1, h24_nu2 = map(int, h24.groups())
        h26_nu1, h26_nu2 = map(int, h26.groups())
        h24_packed_db_records = 1 << (h24_nu1 + h24_nu2)
        h26_packed_db_records = 1 << (h26_nu1 + h26_nu2)
        self.assertEqual(h24_packed_db_records, 1 << 16)
        self.assertEqual(h26_packed_db_records, 1 << 18)
        self.assertEqual(h26_packed_db_records // h24_packed_db_records, 4)
        self.assertEqual(h24_packed_db_records * (2048 // 64), 1 << 21)
        self.assertEqual(h26_packed_db_records * (2048 // 64), 1 << 23)
        self.assertIn("packed_bucket_records", report_rs)
        self.assertIn("2048 / shape.d3", report_rs)
        self.assertIn("params_query_size", report_rs)
        self.assertIn("params_response_size", report_rs)

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
