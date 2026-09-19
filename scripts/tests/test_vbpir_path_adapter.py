import json
import os
import pathlib
import subprocess
import sys
import tempfile
import unittest


REPO = pathlib.Path(__file__).resolve().parents[2]
sys.path.insert(0, str(REPO / "scripts"))

from path_baselines.workload import path_indices, record_bytes
from path_baselines.report import validate_result

ADAPTER = REPO / "scripts" / "path_baselines" / "vbpir_pbc"
SEAL_PREFIX = (
    REPO.parent
    / "generic-superrecord"
    / ".benchmarks"
    / "seal-install-x86_64"
)


class VBPIRPathAdapterTests(unittest.TestCase):
    def build_adapter(self, root):
        build = root / "build"
        env = os.environ.copy()
        env["SEAL_DIR"] = str(SEAL_PREFIX / "lib" / "cmake" / "SEAL-4.0")
        subprocess.run(
            ["cmake", "-S", str(ADAPTER), "-B", str(build)],
            check=True,
            text=True,
            capture_output=True,
            env=env,
        )
        subprocess.run(
            ["cmake", "--build", str(build), "-j2"],
            check=True,
            text=True,
            capture_output=True,
        )
        binary = build / "vbpir_path_adapter"
        self.assertTrue(binary.is_file(), binary)
        return binary

    def test_tiny_encrypted_path_run_returns_exact_ancestor_records(self):
        with tempfile.TemporaryDirectory(prefix="vbpir-path-adapter.") as tmp:
            root = pathlib.Path(tmp)
            binary = self.build_adapter(root)
            leaf_file = root / "leaves.txt"
            output = root / "result.json"
            requested_leaves = [0, 17, 23]
            leaf_file.write_text("\n".join(str(leaf) for leaf in requested_leaves) + "\n")

            subprocess.run(
                [
                    str(binary),
                    "--height",
                    "8",
                    "--leaf-file",
                    str(leaf_file),
                    "--warmup",
                    "1",
                    "--trials",
                    "2",
                    "--first-dim",
                    "128",
                    "--output",
                    str(output),
                ],
                check=True,
                text=True,
                capture_output=True,
            )

            data = json.loads(output.read_text())
            self.assertEqual(data["schema_version"], "path-baseline-v1")
            self.assertEqual(data["scheme"], "vbpir_pbc")
            self.assertEqual(data["path_mode"], "ancestor_root_included")
            self.assertEqual(data["height"], 8)
            self.assertEqual(data["record_bytes"], 32)
            self.assertEqual(data["logical_records"], (1 << 9) - 1)
            self.assertEqual(data["batch_size"], 9)
            self.assertEqual(data["warmups"], 1)
            self.assertEqual(data["trials"], 2)
            self.assertEqual(data["communication"]["query"], "actual_serialized")
            self.assertEqual(data["communication"]["response"], "actual_serialized")
            self.assertEqual(data["communication"]["public_batch_padding"], 5)
            self.assertGreater(data["setup"]["preprocess_ms"], 0)
            self.assertGreater(data["setup"]["bucket_map_ms"], 0)
            self.assertGreater(data["setup"]["replication_bytes"], 0)
            self.assertGreater(data["setup"]["helper_key_bytes"], 0)
            self.assertEqual(data["source_commit"], "930063c5aefc441244abb4890fdf35383f3aa956")
            self.assertEqual(data["profiling_coverage"], "exclusive_stages")
            self.assertIn("coeff_modulus_bits", data["parameters"])
            self.assertEqual(data["parameters"]["requested_first_dimension_size"], 128)
            self.assertEqual(data["parameters"]["first_dimension_size"], 128)
            self.assertEqual(data["parameters"]["padded_matrix_cells"], data["backend_records"])
            self.assertGreaterEqual(
                data["parameters"]["padded_matrix_cells"],
                data["parameters"]["physical_replica_entries"],
            )
            self.assertGreater(data["backend_records"], data["logical_records"])
            self.assertEqual(len(data["warmup_samples"]), 1)
            self.assertTrue(data["warmup_samples"][0]["correct"])
            self.assertEqual(data["warmup_samples"][0]["leaf"], 0)
            validate_result(data, height=8, leaves=requested_leaves, warmups=1, trials=2)

            self.assertEqual(len(data["samples"]), 2)
            for sample, leaf in zip(data["samples"], [17, 23]):
                expected_indices = path_indices(8, leaf)
                self.assertTrue(sample["correct"], sample)
                self.assertEqual(sample["leaf"], leaf)
                self.assertEqual(sample["path_entries"], 9)
                self.assertEqual(sample["correct_entries"], 9)
                self.assertEqual(sample["path_indices"], expected_indices)
                self.assertEqual(sample["decoded_path_indices"], expected_indices)
                self.assertEqual(
                    sample["decoded_path_hex"],
                    [record_bytes(index).hex() for index in expected_indices],
                )
                self.assertGreater(sample["query_ms"], 0)
                self.assertGreater(sample["server_ms"], 0)
                self.assertGreater(sample["decode_ms"], 0)
                self.assertGreater(sample["query_bytes"], 0)
                self.assertGreater(sample["response_bytes"], 0)
                self.assertEqual(
                    set(sample["stages_ms"]),
                    {"query_expansion", "first_dimension_multiply",
                     "dimension_reduction", "response_compression"},
                )

    def test_leaf_file_must_have_distinct_warmup_and_measured_prefix(self):
        with tempfile.TemporaryDirectory(prefix="vbpir-path-adapter-short.") as tmp:
            root = pathlib.Path(tmp)
            binary = self.build_adapter(root)
            leaf_file = root / "leaves.txt"
            output = root / "result.json"
            leaf_file.write_text("0\n17\n")

            proc = subprocess.run(
                [
                    str(binary),
                    "--height",
                    "8",
                    "--leaf-file",
                    str(leaf_file),
                    "--warmup",
                    "1",
                    "--trials",
                    "2",
                    "--output",
                    str(output),
                ],
                text=True,
                capture_output=True,
            )

            self.assertNotEqual(proc.returncode, 0)
            self.assertIn("fewer than warmup + trial leaves", proc.stderr)
            self.assertFalse(output.exists())


if __name__ == "__main__":
    unittest.main()
