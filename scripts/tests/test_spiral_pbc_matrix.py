import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import run_spiral_pbc_matrix as matrix


def fake_result(height, trials, *, correct=True):
    return {
        "scheme": "spiral_pbc", "height": height, "measured_trials": trials, "warmups": 3, "trial_seed": 7,
        "samples": [{"correct": correct, "server_total_ms": 5.0} for _ in range(trials)],
        "measured_summary": {"all_correct": correct, "server_ms_mean": 5.0},
        "communication": {"query_bytes_per_path": 10, "response_bytes_per_path": 20, "public_parameter_bytes": 30},
    }


class SpiralPbcMatrixTests(unittest.TestCase):
    def run_matrix(self, heights, *, fail_heights=(), bad_heights=()):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        root = Path(tmp.name)
        calls, gates = [], []

        def runner(command, *, log_path, env):
            calls.append((command, env))
            height = int(command[command.index("--height") + 1])
            out = Path(command[command.index("--output") + 1])
            if height in fail_heights:
                return {"exit_code": 137}
            out.write_text(json.dumps(fake_result(height, 8, correct=height not in bad_heights)))
            return {"exit_code": 0, "process_peak_rss_bytes": 1}

        code = matrix.main(["--root", str(root), "--heights", *map(str, heights)], runner=runner,
                           gate=lambda cmd, check: gates.append(cmd))
        return root, code, calls, gates

    def test_each_height_is_gated_pinned_single_threaded_and_validated(self):
        root, code, calls, gates = self.run_matrix([22, 26])
        self.assertEqual(code, 0)
        self.assertEqual(len(gates), 2)
        for command, env in calls:
            self.assertEqual(command[:3], ["numactl", "--physcpubind=0", "--preferred=0"])
            self.assertEqual(env["RAYON_NUM_THREADS"], "1")
            self.assertEqual(command[command.index("--trials") + 1], "8")
        progress = json.loads((root / "metadata/spiral-pbc-progress.json").read_text())
        self.assertEqual(progress["state"], "complete")
        self.assertEqual(progress["heights"]["26"]["server_ms_mean"], 5.0)
        self.assertTrue((root / "outputs/spiral-pbc/4gb/spiral_pbc-process.json").exists())

    def test_crash_or_incorrect_result_fails_its_height_and_the_rest_continue(self):
        root, code, calls, _ = self.run_matrix([22, 23, 24], fail_heights={22}, bad_heights={23})
        self.assertEqual(code, 1)
        self.assertEqual(len(calls), 3)
        progress = json.loads((root / "metadata/spiral-pbc-progress.json").read_text())
        self.assertEqual(progress["heights"]["22"]["state"], "failed")
        self.assertIn("137", progress["heights"]["22"]["error"])
        self.assertEqual(progress["heights"]["23"]["state"], "failed")
        self.assertEqual(progress["heights"]["24"]["state"], "complete")
        self.assertEqual(progress["state"], "complete_with_failures")

    def test_unsupported_height_is_rejected(self):
        with self.assertRaises(SystemExit):
            matrix.parse_args(["--root", "/tmp/x", "--heights", "21"])


if __name__ == "__main__":
    unittest.main()
