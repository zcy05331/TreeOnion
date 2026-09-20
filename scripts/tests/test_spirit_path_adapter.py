#!/usr/bin/env python3
import json
import os
import shutil
import subprocess
import tempfile
import unittest
from pathlib import Path

from scripts.path_baselines.report import validate_result


ROOT = Path(__file__).resolve().parents[2]
HARNESS = Path(os.environ.get(
    "SPIRIT_PATH_ADAPTER",
    ROOT / ".benchmarks" / "build-spirit-local" / "spirit_path_adapter",
))


class SpiritPathAdapterSmoke(unittest.TestCase):
    def run_harness(self, args):
        return subprocess.run(
            [str(HARNESS), *args],
            cwd=ROOT,
            text=True,
            capture_output=True,
            check=True,
        )

    def test_default_scheme_runs_spirit_rc_real_encrypted_path_query(self):
        with tempfile.TemporaryDirectory() as td:
            td_path = Path(td)
            leaves = td_path / "leaves.txt"
            out = td_path / "out.json"
            leaves.write_text("0\n3\n", encoding="utf-8")
            self.run_harness([
                "--height", "2",
                "--leaf-file", str(leaves),
                "--warmup", "1",
                "--trials", "1",
                "--output", str(out),
            ])
            result = json.loads(out.read_text(encoding="utf-8"))

        self.assertEqual(result["schema_version"], "path-baseline-v1")
        self.assertEqual(result["scheme"], "spirit_rc")
        self.assertEqual(result["path_mode"], "ancestor_root_included")
        self.assertEqual(result["height"], 2)
        self.assertEqual(result["record_bytes"], 32)
        self.assertEqual(result["logical_records"], 7)
        self.assertGreaterEqual(result["backend_records"], result["logical_records"])
        self.assertEqual(result["layout"]["physical_copied_entries"],
                         result["logical_records"] * result["parameters"]["num_hash_funcs"])
        self.assertEqual(result["layout"]["matrix_32b_equivalent_records"], result["backend_records"])
        self.assertEqual(result["layout"]["matrix_byte_cells"],
                         result["layout"]["matrix_32b_equivalent_records"] * result["record_bytes"])
        self.assertEqual(result["batch_size"], 4)
        self.assertEqual(result["samples"][0]["path_indices"], [0, 2, 6])
        self.assertEqual(len(result["samples"][0]["query_indices"]), 4)
        self.assertEqual(result["samples"][0]["correct_entries"], 3)
        self.assertEqual(result["samples"][0]["path_entries"], 3)
        self.assertTrue(result["measured_summary"]["all_correct"])
        self.assertGreater(result["measured_summary"]["actual_query_payload_bytes_mean"], 0)
        self.assertGreater(result["measured_summary"]["actual_response_payload_bytes_mean"], 0)
        self.assertIsNone(result["setup"]["helper_bytes"])
        self.assertEqual(result["setup"]["helper_bytes_model"], "unavailable_no_setup_serializer")
        self.assertGreater(result["setup"]["public_metadata_bytes"], 0)
        self.assertIn("current_in_memory_public_metadata",
                      result["setup"]["public_metadata_bytes_model"])
        self.assertGreater(result["setup"]["pbc_routing_table_bytes"], 0)
        self.assertGreater(result["setup"]["bucket_layout_metadata_bytes"], 0)
        self.assertGreater(result["setup"]["public_matrix_seed_bytes"], 0)
        self.assertIn("SPIRIT+RC prototype", result["limitations"][0])
        self.assertFalse(result["parameters"]["benchmark_randomize_hint_matrix"])
        self.assertGreaterEqual(len(result["parameters"]["coeff_modulus"]), 1)
        self.assertGreater(result["parameters"]["coeff_modulus"][0], 0)
        self.assertEqual(result["parameters"]["lwe_modulus_bits"], 32)
        self.assertEqual(result["parameters"]["seal_security_level"], "none")
        stages = result["samples"][0]["stages_ms"]
        for stage in ["query_precompute", "secret_unpack", "matvec", "modswitch", "b_extract", "lwe_pack", "answer_glue"]:
            self.assertIn(stage, stages)
            self.assertGreaterEqual(stages[stage], 0)
        self.assertIn("selector_unpack", stages)
        self.assertIn("response_rotation", stages)
        self.assertIn("response_merge", stages)
        self.assertEqual(result["layout"]["variant"], "rc_strided_one_polynomial_per_bucket")
        self.assertGreaterEqual(result["samples"][0]["details"]["response_noise_budget_min_bits"], 0)

    def test_validate_input_only_checks_full_root_included_path(self):
        with tempfile.TemporaryDirectory() as td:
            td_path = Path(td)
            leaves = td_path / "leaves.txt"
            out = td_path / "out.json"
            leaves.write_text("5\n", encoding="utf-8")
            self.run_harness([
                "--height", "3",
                "--leaf-file", str(leaves),
                "--warmup", "0",
                "--trials", "1",
                "--output", str(out),
                "--validate-input-only",
            ])
            result = json.loads(out.read_text(encoding="utf-8"))

        self.assertEqual(result["run_mode"], "validate_input_only")
        self.assertEqual(result["samples"][0]["path_indices"], [0, 2, 5, 12])
        self.assertEqual(result["samples"][0]["query_indices"], [0, 2, 5, 12])

    def test_spirit_rc_real_encrypted_path_query(self):
        with tempfile.TemporaryDirectory() as td:
            td_path = Path(td)
            leaves = td_path / "leaves.txt"
            leaves.write_text("17\n23\n", encoding="utf-8")
            out = td_path / "out.json"
            self.run_harness([
                "--scheme", "spirit_rc",
                "--height", "8",
                "--leaf-file", str(leaves),
                "--warmup", "1",
                "--trials", "1",
                "--output", str(out),
            ])
            result = json.loads(out.read_text(encoding="utf-8"))

        validate_result(result, height=8, leaves=[17, 23], warmups=1, trials=1)
        sample = result["samples"][0]
        self.assertEqual(result["scheme"], "spirit_rc")
        self.assertGreaterEqual(result["backend_records"], result["layout"]["physical_copied_entries"])
        self.assertEqual(result["layout"]["matrix_32b_equivalent_records"], result["backend_records"])
        self.assertEqual(result["layout"]["matrix_byte_cells"],
                         result["layout"]["matrix_32b_equivalent_records"] * result["record_bytes"])
        self.assertEqual(sample["correct_entries"], 9)
        self.assertEqual(sample["path_entries"], 9)
        self.assertEqual(sample["details"]["response_ciphertexts"], 1)
        self.assertGreater(sample["details"]["query_rc_selector_bytes"], 0)
        self.assertGreater(sample["details"]["query_rc_galois_key_bytes"], 0)
        self.assertGreater(sample["details"]["query_rc_selector_ciphertext_bytes"], 0)
        self.assertGreater(sample["details"]["query_rc_selector_parameter_bytes"], 0)
        self.assertGreater(sample["details"]["query_rc_selector_key_bytes"], 0)
        self.assertGreater(sample["details"]["query_rc_merge_galois_key_bytes"], 0)
        self.assertEqual(sample["details"]["query_rc_selector_bucket_count"], result["layout"]["bucket_count"])
        self.assertGreater(sample["details"]["query_rc_selector_value_bits"], 0)
        self.assertGreater(sample["details"]["rc_rotation_external_products"], 0)
        self.assertGreater(sample["details"]["rc_merge_keyswitches"], 0)
        self.assertIn("selector_unpack", sample["stages_ms"])
        self.assertIn("response_rotation", sample["stages_ms"])
        self.assertIn("response_merge", sample["stages_ms"])
        self.assertIn("packed_single_rgsw_query_ciphertext",
                      result["parameters"]["rc_selector_query_packing"])
        self.assertEqual(result["parameters"]["rc_expansion_coeff_modulus_bits"], [56, 60, 60])
        self.assertEqual(len(result["parameters"]["rc_expansion_coeff_modulus"]), 3)
        self.assertEqual(result["layout"]["variant"], "rc_strided_one_polynomial_per_bucket")

    def test_rejects_randomized_hint_escape_hatch_and_bad_schedule(self):
        with tempfile.TemporaryDirectory() as td:
            td_path = Path(td)
            leaves = td_path / "leaves.txt"
            leaves.write_text("0\n", encoding="utf-8")

            zero_trials = subprocess.run(
                [str(HARNESS), "--height", "2", "--leaf-file", str(leaves), "--warmup", "0", "--trials", "0"],
                cwd=ROOT,
                text=True,
                capture_output=True,
            )
            self.assertNotEqual(zero_trials.returncode, 0)
            self.assertIn("--trials must be greater than zero", zero_trials.stderr)

            randomized = subprocess.run(
                [
                    str(HARNESS),
                    "--height", "2",
                    "--leaf-file", str(leaves),
                    "--warmup", "0",
                    "--trials", "1",
                    "--benchmark-randomize-hint-matrix", "1",
                ],
                cwd=ROOT,
                text=True,
                capture_output=True,
            )
            self.assertNotEqual(randomized.returncode, 0)
            self.assertIn("unknown option", randomized.stderr)

    def test_rejects_removed_standalone_spirit_scheme(self):
        with tempfile.TemporaryDirectory() as td:
            leaves = Path(td) / "leaves.txt"
            leaves.write_text("0\n", encoding="utf-8")
            standalone = subprocess.run(
                [
                    str(HARNESS),
                    "--scheme", "spirit",
                    "--height", "2",
                    "--leaf-file", str(leaves),
                    "--warmup", "0",
                    "--trials", "1",
                    "--validate-input-only",
                ],
                cwd=ROOT,
                text=True,
                capture_output=True,
            )
            self.assertNotEqual(standalone.returncode, 0)
            self.assertIn("SPIRIT standalone baseline has been removed", standalone.stderr)

    def test_cmake_requires_exact_source_commit_for_source_archive(self):
        with tempfile.TemporaryDirectory() as td:
            td_path = Path(td)
            archive = td_path / "SPIRIT-archive"
            shutil.copytree(ROOT / ".benchmarks" / "SPIRIT" / "src", archive / "src")

            missing = subprocess.run(
                [
                    "cmake",
                    "-S", str(ROOT / "scripts" / "path_baselines" / "spirit"),
                    "-B", str(td_path / "build-missing"),
                    f"-DSPIRIT_DIR={archive}",
                ],
                cwd=ROOT,
                text=True,
                capture_output=True,
            )
            self.assertNotEqual(missing.returncode, 0)
            self.assertIn("SPIRIT_DIR has no git metadata", missing.stderr + missing.stdout)

            malformed = subprocess.run(
                [
                    "cmake",
                    "-S", str(ROOT / "scripts" / "path_baselines" / "spirit"),
                    "-B", str(td_path / "build-malformed"),
                    f"-DSPIRIT_DIR={archive}",
                    "-DSPIRIT_SOURCE_COMMIT=unknown",
                ],
                cwd=ROOT,
                text=True,
                capture_output=True,
            )
            self.assertNotEqual(malformed.returncode, 0)
            self.assertIn("SPIRIT_SOURCE_COMMIT must be a full 40-hex commit",
                          malformed.stderr + malformed.stdout)

            valid = subprocess.run(
                [
                    "cmake",
                    "-S", str(ROOT / "scripts" / "path_baselines" / "spirit"),
                    "-B", str(td_path / "build-valid"),
                    f"-DSPIRIT_DIR={archive}",
                    "-DSPIRIT_SOURCE_COMMIT=0123456789abcdef0123456789abcdef01234567",
                ],
                cwd=ROOT,
                text=True,
                capture_output=True,
            )
            self.assertNotIn("SPIRIT_SOURCE_COMMIT must be a full 40-hex commit",
                             valid.stderr + valid.stdout)
            self.assertNotIn("SPIRIT_DIR has no git metadata", valid.stderr + valid.stdout)

    def test_cmake_rejects_source_commit_that_mislabels_git_checkout(self):
        with tempfile.TemporaryDirectory() as td:
            mismatch = subprocess.run(
                [
                    "cmake",
                    "-S", str(ROOT / "scripts" / "path_baselines" / "spirit"),
                    "-B", str(Path(td) / "build-mismatch"),
                    f"-DSPIRIT_DIR={ROOT / '.benchmarks' / 'SPIRIT'}",
                    "-DSPIRIT_SOURCE_COMMIT=0000000000000000000000000000000000000000",
                ],
                cwd=ROOT,
                text=True,
                capture_output=True,
            )
            self.assertNotEqual(mismatch.returncode, 0)
            self.assertIn("SPIRIT_SOURCE_COMMIT does not match git HEAD",
                          mismatch.stderr + mismatch.stdout)


if __name__ == "__main__":
    unittest.main()
