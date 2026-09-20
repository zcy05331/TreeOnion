from __future__ import annotations

import json
import pathlib
import sys
import tempfile
import unittest
from io import StringIO
from unittest.mock import patch


sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import benchmark_tree_multiplane as bench


LEAVES = 1 << 24


def profile(*, g: int, n0: int, experiments: int, warmup: int = 3,
            mean: float = 10.0, correct: bool = True, plane_count: int | None = None,
            chunk_bits: int = 12, leaf_offset: int = 100) -> dict:
    if plane_count is None:
        plane_count = (256 + chunk_bits * g - 1) // (chunk_bits * g)
    ring_degree = 2048
    rho = ring_degree // g
    b = (LEAVES // (rho * n0)).bit_length() - 1
    samples = [
        {
            "trial": idx,
            "leaf": leaf_offset + idx,
            "server_total_ms": mean + idx,
            "client_query_ms": 1.0,
            "response_serialize_ms": 1.0,
            "actual_response_bytes": 2048,
            "client_decode_ms": 1.0,
            "stages_ms": {"scan_core": 1.0, "scan_finalize": 1.0, "pack": 1.0},
            "diagnostics_ms": {"scan_combined": 2.0},
            "correct": correct,
        }
        for idx in range(experiments)
    ]
    return {
        "scheme": "tree-multiplane",
        "leaf_count": LEAVES,
        "node_entry_bits": 256,
        "actual_logical_bytes": (2 * LEAVES - 1) * 32,
        "actual_logical_bytes_scope": "full_tree_all_nodes",
        "warmups": warmup,
        "measured_trials": experiments,
        "trial_seed": 7,
        "offline_setup_ms": 42.0,
        "diagnostic_non_comparable_32B_payload": False,
        "mode": "mlwe",
        "metadata": {
            "index_layout": "gamma-alpha-beta",
            "h": "1",
            "g": str(g),
            "plane_count": str(plane_count),
            "N0": str(n0),
            "b": str(b),
            "rho": str(rho),
            "ring_degree": str(ring_degree),
            "chunk_bits": str(chunk_bits),
            "response_packing": "mlwe",
            "query_layout": "interleaved",
        },
        "communication": {
            "modeled_query_bytes_per_trial": 1024,
            "actual_response_bytes_per_trial": 2048,
            "modeled_helper_key_bytes": 4096,
            "modeled_compression_key_bytes": 0,
            "query_bytes_kind": "modeled",
            "response_bytes_kind": "actual",
            "helper_key_bytes_kind": "modeled",
            "compression_key_bytes_kind": "modeled",
        },
        "samples": samples,
    }


def run_args(root: pathlib.Path, *extra: str) -> list[str]:
    return [
        "--binary",
        "/tmp/tree-multiplane-bin",
        "--output-dir",
        str(root),
        "--leaf-count",
        str(LEAVES),
        "--n0-values",
        "256,512",
        *extra,
    ]


class TreeMultiplaneBenchmarkTests(unittest.TestCase):
    def test_utilization_first_with_plane_cap_and_minimum_plane_tie_break(self):
        expected = {128: (4, 3), 256: (8, 3), 384: (32, 1),
                    512: (16, 3), 1024: (32, 3), 480: (8, 5)}
        for bits, shape in expected.items():
            self.assertEqual(bench.select_layout(bits), shape)
        self.assertEqual(bench.select_layout(256, 8), (32, 1))
        with self.assertRaises(bench.SweepError):
            bench.select_layout(100000)

    def test_plan_only_prints_exact_cases_without_running_processes(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            with patch.object(sys, "stdout") as stdout, patch.object(bench, "run_measured") as run_measured:
                self.assertEqual(bench.main(run_args(pathlib.Path(tmp), "--plan-only")), 0)

        run_measured.assert_not_called()
        plan = json.loads("".join(call.args[0] for call in stdout.write.call_args_list))
        cases = plan["cases"]
        self.assertEqual([case["label"] for case in cases], ["g8-n0256", "g8-n0512"])
        self.assertEqual(cases[0]["command"][0], str(pathlib.Path("/tmp/tree-multiplane-bin").resolve()))
        self.assertEqual(cases[0]["command"][1:3], ["--test", "tree_multiplane_bench"])
        self.assertEqual(cases[0]["environment"]["TREE_MULTIPLANE_G"], "8")
        self.assertEqual(cases[0]["environment"]["TREE_MULTIPLANE_N0"], "256")
        self.assertEqual(cases[0]["environment"]["TREE_MULTIPLANE_PACKING"], "mlwe")

    def test_serial_success_ranks_complete_cases_by_mean_server_time(self) -> None:
        launches = []

        def fake_run(command, *, log_path, env=None, cwd=None):
            launches.append({"command": command, "env": env.copy(), "log": log_path})
            raw = pathlib.Path(command[command.index("--benchmark-json") + 1])
            g = int(env["TREE_MULTIPLANE_G"])
            n0 = int(env["TREE_MULTIPLANE_N0"])
            raw.write_text(json.dumps(profile(g=g, n0=n0, experiments=3, mean=20.0 if n0 == 256 else 5.0)) + "\n")
            pathlib.Path(log_path).write_text("ok\n")
            return {"exit_code": 0, "elapsed_seconds": 0.1, "process_peak_rss_bytes": 1234}

        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            with patch.object(bench, "run_measured", fake_run), patch.object(sys, "stdout", StringIO()):
                self.assertEqual(bench.main(run_args(root)), 0)

            self.assertEqual([item["env"]["TREE_MULTIPLANE_G"] for item in launches], ["8", "8"])
            self.assertEqual(launches[0]["command"][launches[0]["command"].index("--experiments") + 1], "3")
            self.assertEqual(launches[0]["command"][launches[0]["command"].index("--warmup") + 1], "3")
            ranking = json.loads((root / "ranking.json").read_text())
            self.assertEqual([row["label"] for row in ranking["completed"]], ["g8-n0512", "g8-n0256"])
            self.assertEqual(ranking["completed"][0]["metadata"]["index_layout"], "gamma-alpha-beta")
            manifest = json.loads((root / "manifest.json").read_text())
            self.assertEqual(manifest["state"], "complete")

    def test_process_failure_is_preserved_and_excluded_from_ranking(self) -> None:
        def fake_run(command, *, log_path, env=None, cwd=None):
            raw = pathlib.Path(command[command.index("--benchmark-json") + 1])
            pathlib.Path(log_path).write_text("ran\n")
            if env["TREE_MULTIPLANE_N0"] == "256":
                return {"exit_code": 4, "elapsed_seconds": 0.1, "process_peak_rss_bytes": 5}
            raw.write_text(json.dumps(profile(g=8, n0=512, experiments=3, mean=3.0)) + "\n")
            return {"exit_code": 0, "elapsed_seconds": 0.2, "process_peak_rss_bytes": 6}

        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            with patch.object(bench, "run_measured", fake_run), patch.object(sys, "stdout", StringIO()):
                self.assertEqual(bench.main(run_args(root)), 0)

            manifest = json.loads((root / "manifest.json").read_text())
            self.assertEqual(manifest["state"], "complete_with_failures")
            self.assertEqual([row["state"] for row in manifest["cases"]], ["failed", "complete"])
            self.assertTrue((root / "g8-n0256-process.json").exists())
            self.assertTrue((root / "g8-n0256.log").exists())
            ranking = json.loads((root / "ranking.json").read_text())
            self.assertEqual([row["label"] for row in ranking["completed"]], ["g8-n0512"])
            self.assertEqual(ranking["failed"][0]["label"], "g8-n0256")

    def test_invalid_shape_metadata_fails_case_without_reusing_as_complete(self) -> None:
        def fake_run(command, *, log_path, env=None, cwd=None):
            raw = pathlib.Path(command[command.index("--benchmark-json") + 1])
            raw.write_text(json.dumps(profile(g=8, n0=256, experiments=3, plane_count=99)) + "\n")
            pathlib.Path(log_path).write_text("bad metadata\n")
            return {"exit_code": 0, "elapsed_seconds": 0.1, "process_peak_rss_bytes": 1}

        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            with patch.object(bench, "run_measured", fake_run), patch.object(sys, "stderr", StringIO()):
                self.assertEqual(bench.main(run_args(root, "--n0-values", "256")), 1)

            manifest = json.loads((root / "manifest.json").read_text())
            self.assertEqual(manifest["state"], "failed")
            self.assertIn("plane_count", manifest["cases"][0]["error"])
            ranking = json.loads((root / "ranking.json").read_text())
            self.assertEqual(ranking["completed"], [])

    def test_rejects_existing_case_output_before_launch(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            root.mkdir(exist_ok=True)
            (root / "g8-n0256.json").write_text("{}\n")
            with patch.object(bench, "run_measured") as run_measured, patch.object(sys, "stderr", StringIO()):
                self.assertEqual(bench.main(run_args(root, "--n0-values", "256")), 1)

        run_measured.assert_not_called()

    def test_sample_leaves_must_match_first_accepted_case(self) -> None:
        def fake_run(command, *, log_path, env=None, cwd=None):
            raw = pathlib.Path(command[command.index("--benchmark-json") + 1])
            g = int(env["TREE_MULTIPLANE_G"])
            raw.write_text(json.dumps(profile(g=g, n0=int(env["TREE_MULTIPLANE_N0"]), experiments=3, leaf_offset=100 if env["TREE_MULTIPLANE_N0"] == "256" else 200)) + "\n")
            pathlib.Path(log_path).write_text("ok\n")
            return {"exit_code": 0, "elapsed_seconds": 0.1, "process_peak_rss_bytes": 1}

        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            with patch.object(bench, "run_measured", fake_run), patch.object(sys, "stdout", StringIO()):
                self.assertEqual(bench.main(run_args(root)), 0)

            manifest = json.loads((root / "manifest.json").read_text())
            self.assertEqual([row["state"] for row in manifest["cases"]], ["complete", "failed"])
            self.assertIn("sample leaf schedule differs", manifest["cases"][1]["error"])


if __name__ == "__main__":
    unittest.main()
