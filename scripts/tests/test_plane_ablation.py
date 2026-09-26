from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

SCRIPTS = Path(__file__).resolve().parents[1]
sys.path.insert(0, str(SCRIPTS))
import run_plane_ablation as ablation


class PlaneAblationTests(unittest.TestCase):
    def create(self, directory, scales=None):
        root = Path(directory)
        (root / "metadata").mkdir()
        (root / "metadata/source-commit.txt").write_text("test-source\n")
        return ablation.PlaneAblation(root, scales or [scale for scale, _ in ablation.SCALES])

    def test_each_size_runs_the_policy_shape_and_one_plane_with_paper_settings(self):
        with tempfile.TemporaryDirectory() as directory:
            queue = self.create(directory)
            calls = []

            def run(scale, height, phase, g, n0, layout, trials, policy_layout=True):
                calls.append((scale, g, n0, layout, trials, policy_layout))
                return {"state": "complete", "sample_leaves": [7] * trials}

            with patch.object(queue, "run_case", side_effect=run):
                self.assertEqual(queue.run(), 0)
            self.assertEqual(len(calls), 12)
            self.assertEqual({(g, policy) for _, g, _, _, _, policy in calls},
                             {(8, True), (32, False)})
            self.assertTrue(all(trials == 50 for *_, trials, _ in calls))
            by_scale = {scale: (n0, layout) for scale, _, n0, layout, _, _ in calls}
            self.assertEqual(by_scale["256mib"], (256, "split-columns"))
            self.assertEqual(by_scale["4gib"], (512, "split-columns"))
            self.assertEqual(by_scale["8gib"], (512, "interleaved"))
            self.assertEqual(queue.state["state"], "complete")

    def test_mismatched_schedules_stop_the_queue(self):
        with tempfile.TemporaryDirectory() as directory:
            queue = self.create(directory, ["1gib"])

            def run(scale, height, phase, g, n0, layout, trials, policy_layout=True):
                return {"state": "complete", "sample_leaves": [g] * trials}

            with patch.object(queue, "run_case", side_effect=run):
                with self.assertRaises(RuntimeError):
                    queue.run()

    def test_a_failed_shape_marks_the_queue(self):
        with tempfile.TemporaryDirectory() as directory:
            queue = self.create(directory, ["1gib", "2gib"])

            def run(scale, height, phase, g, n0, layout, trials, policy_layout=True):
                state = "failed" if (scale, g) == ("1gib", 32) else "complete"
                return {"state": state, "sample_leaves": [7] * trials}

            with patch.object(queue, "run_case", side_effect=run) as mock:
                self.assertEqual(queue.run(), 1)
            self.assertEqual(mock.call_count, 4)
            self.assertEqual(queue.state["state"], "complete_with_failures")


if __name__ == "__main__":
    unittest.main()
