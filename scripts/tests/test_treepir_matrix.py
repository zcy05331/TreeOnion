import json
from pathlib import Path
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import run_treepir_matrix as matrix


class TreePirMatrixTests(unittest.TestCase):
    def test_real_argument_parser_accepts_wrapper_and_all_four_sizes(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "metadata").mkdir()
            with patch.object(sys, "argv", ["run_treepir_matrix", "--root", str(root)]), \
                    patch.object(matrix.tuning, "run_one_height", return_value=True) as run:
                self.assertEqual(matrix.main(), 0)
            self.assertEqual([call.args[1] for call in run.call_args_list], [22, 23, 24, 25, 26])
            args = run.call_args_list[0].args[0]
            self.assertEqual((args.trials, args.pilot_trials, args.warmups, args.trial_seed), (8, 8, 3, 7))
            self.assertEqual(json.loads((root / "metadata/treepir-progress.json").read_text())["state"], "complete")

    def test_failed_height_is_retained_and_independent_heights_continue(self):
        with tempfile.TemporaryDirectory() as directory:
            root = Path(directory)
            (root / "metadata").mkdir()
            with patch.object(sys, "argv", ["run_treepir_matrix", "--root", str(root)]), \
                    patch.object(matrix.tuning, "run_one_height", side_effect=[False, True, True, True, True]) as run:
                self.assertEqual(matrix.main(), 1)
            self.assertEqual(run.call_count, 5)
            status = json.loads((root / "metadata/treepir-progress.json").read_text())
            self.assertEqual(status["heights"]["22"]["state"], "failed")
            self.assertEqual(status["state"], "complete_with_failures")


if __name__ == "__main__":
    unittest.main()
