#!/usr/bin/env python3
import json
import os
import subprocess
import tempfile
import unittest
from pathlib import Path

ROOT = Path(__file__).resolve().parents[3]
HARNESS = Path(os.environ.get(
    "TREEPIR_SEALPIR_ADAPTER",
    ROOT / "scripts" / "treepir_sealpir" / "build" / "treepir_sealpir_adapter",
))


def write_index_fixture(path: Path, height: int, leaves, warmups: int = 0):
    def path_nodes_for_leaf(h, leaf_id):
        node = (1 << h) + leaf_id
        out = [0] * h
        for i in range(h - 1, -1, -1):
            out[i] = node
            node //= 2
        return out

    # These sub_indices are small hand fixtures. The adapter test validates schema
    # plumbing and decode behavior; production VBPIR JSON supplies full indices.
    results = []
    for i, leaf in enumerate(leaves, 1):
        results.append({
            "iteration": i,
            "phase": "warmup" if i <= warmups else "measured",
            "leaf_id": leaf,
            "path_nodes": path_nodes_for_leaf(height, leaf),
            "sub_indices": list(range(height)),
            "correct": True,
        })
    data = {
        "model": "SEAL+VBPIRTreePIR",
        "height": height,
        "leaf_count": 1 << height,
        "entry_size_bytes": 32,
        "layout": {
            "color_buckets": height,
            "max_bucket_entries": max(height + 4, 8),
        },
        "warmups": warmups,
        "trials": max(0, len(leaves) - warmups),
        "validate_index_only": True,
        "results": results,
    }
    path.write_text(json.dumps(data), encoding="utf-8")


class TreePirSealPirAdapterSmoke(unittest.TestCase):
    def run_harness(self, args):
        return subprocess.run([str(HARNESS), *args], cwd=ROOT, text=True, capture_output=True, check=True)

    def test_h10_decodes_all_path_entries(self):
        with tempfile.TemporaryDirectory() as td:
            td = Path(td)
            inp = td / "h10.json"
            out = td / "out.json"
            write_index_fixture(inp, 10, [0, 17], warmups=1)
            self.run_harness(["--input", str(inp), "--output", str(out), "--trials", "1", "--warmup", "1", "--max-color-entries", "10"])
            result = json.loads(out.read_text(encoding="utf-8"))
        self.assertEqual(result["model"], "TreePIR+SealPIRplusCore")
        self.assertTrue(result["measured_summary"]["all_correct"])
        self.assertEqual(result["height"], 10)
        self.assertEqual(result["entry_size_bytes"], 32)
        self.assertEqual(result["layout"]["color_buckets"], 10)
        self.assertGreater(result["measured_summary"]["actual_response_payload_bytes_mean"], 0)
        self.assertEqual(result["transport_mode"], "manual_uncompressed_ciphertext_save")
        self.assertEqual(result["query_payload_label"], "actual_existing_query_ciphertexts_save_none")
        self.assertEqual(result["response_payload_label"], "server_modswitch_then_manual_ciphertext_save_none")
        self.assertTrue(result["layout"]["smoke_cap_active"])
        self.assertEqual(result["layout"]["effective_max_bucket_entries"], 10)
        self.assertEqual(len(result["results"][0]["server_color_ms"]), 10)
        self.assertEqual(len(result["results"][0]["server_generate_reply_color_ms"]), 10)
        self.assertEqual(len(result["results"][0]["server_modswitch_color_ms"]), 10)
        self.assertAlmostEqual(
            result["results"][0]["server_ms"],
            result["results"][0]["server_generate_reply_ms"] + result["results"][0]["server_modswitch_ms"],
            delta=0.010,
        )
        self.assertLess(
            result["results"][0]["server_modswitch_ms"],
            result["results"][0]["server_generate_reply_ms"],
        )
        stage_names = [
            "server_expansion_ms",
            "server_query_ntt_ms",
            "server_database_ntt_ms",
            "server_multiply_accumulate_ms",
            "server_inverse_ntt_ms",
            "server_recursive_decomposition_modswitch_ms",
        ]
        stage_sum = 0.0
        for name in stage_names:
            self.assertIn(name, result["results"][0])
            self.assertGreaterEqual(result["results"][0][name], 0.0)
            self.assertEqual(len(result["results"][0][name.replace("_ms", "_color_ms")]), 10)
            stage_sum += result["results"][0][name]
        self.assertLessEqual(stage_sum, result["results"][0]["server_generate_reply_ms"] + 0.010)
        self.assertIn("server_expansion_ms", result["stage_definitions"])

    def test_rejects_explicit_zero_and_insufficient_schedule(self):
        with tempfile.TemporaryDirectory() as td:
            td = Path(td)
            inp = td / "h10.json"
            write_index_fixture(inp, 10, [0], warmups=0)

            zero = subprocess.run([str(HARNESS), "--input", str(inp), "--trials", "0"], cwd=ROOT, text=True, capture_output=True)
            self.assertNotEqual(zero.returncode, 0)
            self.assertIn("--trials must be greater than zero when specified", zero.stderr)

            too_many = subprocess.run([str(HARNESS), "--input", str(inp), "--warmup", "1", "--trials", "1", "--max-color-entries", "16"], cwd=ROOT, text=True, capture_output=True)
            self.assertNotEqual(too_many.returncode, 0)
            self.assertIn("requested warmup+trials exceeds input result rows", too_many.stderr)

    def test_h24_h26_h27_validate_input_only(self):
        with tempfile.TemporaryDirectory() as td:
            td = Path(td)
            for height in (24, 26, 27):
                inp = td / f"h{height}.json"
                out = td / f"h{height}.out.json"
                write_index_fixture(inp, height, [0])
                self.run_harness(["--input", str(inp), "--output", str(out), "--validate-input-only"])
                result = json.loads(out.read_text(encoding="utf-8"))
                self.assertEqual(result["height"], height)
                self.assertEqual(result["layout"]["color_buckets"], height)
                self.assertEqual(result["run_mode"], "validate_input_only")


if __name__ == "__main__":
    unittest.main()
