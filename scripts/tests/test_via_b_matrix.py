import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import run_via_b_matrix as matrix

DEFAULT_ROWS = {22: 6, 24: 7, 26: 8}
# server ms by (compiler, log_rows): icpx is faster, one row above the default wins
SPEED = {"gcc": 12.0, "icpx": 10.0}


class ViaBMatrixTests(unittest.TestCase):
    def run_matrix(self, extra=(), fail=frozenset()):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        root = Path(tmp.name)
        calls = []

        def runner(command, *, log_path, env):
            label = command[command.index("--compiler-label") + 1]
            height = int(command[command.index("--height") + 1])
            trials = int(command[command.index("--trials") + 1])
            warmups = int(command[command.index("--warmup") + 1])
            rows = int(command[command.index("--log-rows") + 1]) if "--log-rows" in command else None
            mode = command[command.index("--batch") + 1]
            calls.append((label, height, rows, trials, command))
            if (label, rows) in fail:
                return {"exit_code": 134}
            actual = DEFAULT_ROWS[height] if rows is None else rows
            speed = SPEED[label] - (actual - DEFAULT_ROWS[height] == 1)
            out = Path(command[command.index("--output") + 1])
            out.write_text(json.dumps({
                "scheme": matrix.SCHEME_BY_MODE[mode], "height": height, "measured_trials": trials,
                "warmups": warmups, "trial_seed": 7, "batch": {"mode": mode}, "via_parameters": {"log_rows": actual},
                "samples": [{"correct": True} for _ in range(trials)],
                "measured_summary": {"all_correct": True, "server_ms_mean": speed}}))
            return {"exit_code": 0}

        code = matrix.main(["--root", str(root), "--binary", "gcc=/b/gcc", "--binary", "icpx=/b/icpx", *extra],
                           runner=runner, gate=lambda cmd, check: None)
        return root, code, calls

    def test_faster_compiler_then_neighbouring_row_counts_then_formal(self):
        root, code, calls = self.run_matrix(["--heights", "22", "26"])
        self.assertEqual(code, 0)
        self.assertEqual([c[:4] for c in calls[:2]], [("gcc", 24, None, 2), ("icpx", 24, None, 2)])
        progress = json.loads((root / "metadata/via-b-progress.json").read_text())
        self.assertEqual(progress["chosen_compiler"], "icpx")
        pilots22 = [c[2] for c in calls if c[1] == 22 and c[3] == 2]
        self.assertEqual(pilots22, [None, 5, 7])
        for h in ("22", "26"):
            self.assertEqual(progress["heights"][h]["state"], "complete")
        self.assertEqual(progress["heights"]["22"]["chosen_log_rows"], 7)
        self.assertEqual(progress["heights"]["26"]["chosen_log_rows"], 9)
        formal = [(c[0], c[1], c[2]) for c in calls if c[3] == 8]
        self.assertEqual(formal, [("icpx", 22, 7), ("icpx", 26, 9)])
        for c in calls:
            self.assertEqual(c[4][:3], ["numactl", "--physcpubind=0", "--preferred=0"])
            self.assertEqual(c[4][c[4].index("--batch") + 1], "native")
        self.assertTrue((root / "outputs/via-b/4gb/via_b.json").exists())

    def test_pbc_mode_writes_the_separate_variant(self):
        root, code, calls = self.run_matrix(["--heights", "24", "--batch-mode", "pbc"])
        self.assertEqual(code, 0)
        self.assertTrue(all(c[4][c[4].index("--batch") + 1] == "pbc" for c in calls))
        progress = json.loads((root / "metadata/via-b-pbc-progress.json").read_text())
        self.assertEqual(progress["batch_mode"], "pbc")
        self.assertTrue((root / "outputs/via-b-pbc/1gb/via_b_pbc.json").exists())

    def test_failed_compiler_is_skipped_and_failed_rows_are_not_selected(self):
        root, code, calls = self.run_matrix(["--heights", "24"], fail={("icpx", None), ("gcc", 8)})
        progress = json.loads((root / "metadata/via-b-progress.json").read_text())
        self.assertEqual(progress["chosen_compiler"], "gcc")
        self.assertEqual(progress["compiler_pilot"]["icpx"]["state"], "failed")
        self.assertEqual(progress["heights"]["24"]["pilots"]["8"]["state"], "failed")
        self.assertEqual(progress["heights"]["24"]["chosen_log_rows"], 7)
        self.assertEqual(code, 0)

    def test_bad_binary_spec_and_height_are_rejected(self):
        with self.assertRaises(SystemExit):
            matrix.parse_args(["--root", "/x", "--binary", "gcc"])
        with self.assertRaises(SystemExit):
            matrix.parse_args(["--root", "/x", "--binary", "gcc=/b", "--heights", "21"])


if __name__ == "__main__":
    unittest.main()
