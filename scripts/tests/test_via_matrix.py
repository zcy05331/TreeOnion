import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import run_via_matrix as matrix

DEFAULT_ROWS = {22: 6, 24: 7, 26: 8}
# server ms by (compiler, log_rows): icpx is faster, one row above the default wins
SPEED = {"gcc": 12.0, "icpx": 10.0}


class ViaMatrixTests(unittest.TestCase):
    def run_matrix(self, scheme, extra=(), fail=frozenset()):
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
            calls.append((label, height, rows, trials, command))
            if (label, rows) in fail:
                return {"exit_code": 134}
            actual = DEFAULT_ROWS[height] if rows is None else rows
            speed = SPEED[label] - (actual - DEFAULT_ROWS[height] == 1)
            out = Path(command[command.index("--output") + 1])
            out.write_text(json.dumps({
                "scheme": scheme, "height": height, "measured_trials": trials,
                "warmups": warmups, "trial_seed": 7, "via_parameters": {"log_rows": actual},
                "samples": [{"correct": True} for _ in range(trials)],
                "measured_summary": {"all_correct": True, "server_ms_mean": speed}}))
            return {"exit_code": 0}

        code = matrix.main(["--root", str(root), "--scheme", scheme, "--binary", "gcc=/b/gcc", "--binary", "icpx=/b/icpx",
                            *extra], runner=runner, gate=lambda cmd, check: None)
        return root, code, calls

    def test_faster_compiler_then_neighbouring_row_counts_then_formal(self):
        root, code, calls = self.run_matrix("via_b", ["--heights", "22", "26"])
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
        self.assertTrue((root / "outputs/via-b/4gb/via_b.json").exists())

    def test_via_pbc_writes_its_own_rows(self):
        root, code, calls = self.run_matrix("via_pbc", ["--heights", "24"])
        self.assertEqual(code, 0)
        progress = json.loads((root / "metadata/via-pbc-progress.json").read_text())
        self.assertEqual(progress["scheme"], "via_pbc")
        self.assertTrue((root / "outputs/via-pbc/1gb/via_pbc.json").exists())

    def test_result_of_the_other_scheme_is_rejected(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        path = Path(tmp.name) / "r.json"
        path.write_text(json.dumps({"scheme": "via_b", "height": 24}))
        with self.assertRaises(matrix.ViaMatrixError):
            matrix.validate(path, scheme="via_pbc", height=24, trials=8, warmups=3, seed=7, log_rows=None)

    def test_failed_compiler_is_skipped_and_failed_rows_are_not_selected(self):
        root, code, calls = self.run_matrix("via_b", ["--heights", "24"], fail={("icpx", None), ("gcc", 8)})
        progress = json.loads((root / "metadata/via-b-progress.json").read_text())
        self.assertEqual(progress["chosen_compiler"], "gcc")
        self.assertEqual(progress["compiler_pilot"]["icpx"]["state"], "failed")
        self.assertEqual(progress["heights"]["24"]["pilots"]["8"]["state"], "failed")
        self.assertEqual(progress["heights"]["24"]["chosen_log_rows"], 7)
        self.assertEqual(code, 0)

    def test_bad_binary_spec_height_and_scheme_are_rejected(self):
        for argv in (["--scheme", "via_b", "--binary", "gcc"],
                     ["--scheme", "via_b", "--binary", "gcc=/b", "--heights", "21"],
                     ["--scheme", "via_c", "--binary", "gcc=/b"],
                     ["--binary", "gcc=/b"]):
            with self.subTest(argv=argv), self.assertRaises(SystemExit):
                matrix.parse_args(["--root", "/x", *argv])


if __name__ == "__main__":
    unittest.main()
