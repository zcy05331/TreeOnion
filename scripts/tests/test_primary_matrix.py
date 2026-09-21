import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

SCRIPTS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPTS))
import run_primary_matrix as primary


class PrimaryQueueTests(unittest.TestCase):
    def create(self, directory):
        root = Path(directory)
        (root / "metadata").mkdir()
        (root / "metadata/source-commit.txt").write_text("test-source\n")
        return primary.PrimaryMatrix(root)

    def test_selected_shape_is_reused_for_both_fifty_trial_layouts(self):
        with tempfile.TemporaryDirectory() as directory:
            matrix = self.create(directory)
            calls = []

            def run(scale, height, phase, g, n0, layout, trials):
                calls.append((scale, phase, g, n0, layout, trials))
                return {"state": "complete", "g": g, "N0": n0,
                        "mean_server_total_ms": g + n0 / 1000,
                        "sample_leaves": [7] * trials, "raw_json": "result.json"}

            with patch.object(matrix, "run_case", side_effect=run):
                self.assertEqual(matrix.run(), 0)
            formal = [row for row in calls if row[1] == "formal"]
            self.assertEqual(len(formal), 10)
            self.assertEqual({row[0] for row in formal}, {"256mib", "512mib", "1gib", "2gib", "4gib"})
            self.assertTrue(all(row[2:4] == (8, 128) and row[-1] == 50 for row in formal))
            self.assertEqual({row[4] for row in formal}, {"interleaved", "split-columns"})
            self.assertEqual(len(calls), 30)

    def test_no_correct_pilot_does_not_launch_formal(self):
        with tempfile.TemporaryDirectory() as directory:
            matrix = self.create(directory)
            with patch.object(matrix, "run_case", return_value={"state": "failed"}) as run:
                self.assertEqual(matrix.run(), 1)
            self.assertEqual(run.call_count, 20)
            self.assertEqual(matrix.state["state"], "complete_with_failures")

    def test_idle_gate_precedes_measured_child_and_failed_child_is_retained(self):
        with tempfile.TemporaryDirectory() as directory:
            matrix = self.create(directory)
            (matrix.root / "metadata/idle.json").write_text('{"other_jobs": []}')
            calls = []

            def gate(command, **kwargs):
                calls.append("gate")
                self.assertEqual(command[-2:], ["--root", str(matrix.root)])

            def measured(command, **kwargs):
                calls.append("child")
                self.assertEqual(command[:3], ["numactl", "--physcpubind=0", "--preferred=0"])
                return {"exit_code": 1}

            with patch.object(primary.subprocess, "run", side_effect=gate), \
                    patch.object(primary, "run_measured", side_effect=measured):
                row = matrix.run_case("1gib", 24, "pilots", 8, 512, "interleaved", 8)
            self.assertEqual(calls, ["gate", "child"])
            self.assertEqual(row["state"], "failed")
            self.assertEqual(json.loads(Path(row["process_json"]).read_text())["exit_code"], 1)


if __name__ == "__main__":
    unittest.main()
