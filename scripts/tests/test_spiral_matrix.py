import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import run_spiral_matrix as matrix


def fake_result(scheme, height, trials, *, correct=True):
    return {
        "scheme": scheme, "height": height, "measured_trials": trials, "warmups": 3, "trial_seed": 7,
        "samples": [{"correct": correct, "server_total_ms": 5.0} for _ in range(trials)],
        "measured_summary": {"all_correct": correct, "server_ms_mean": 5.0},
        "communication": {"query_bytes_per_path": 10, "response_bytes_per_path": 20, "public_parameter_bytes": 30},
    }


class SpiralMatrixTests(unittest.TestCase):
    def run_matrix(self, heights, *, scheme="spiral_pbc", extra=(), fail_heights=(), bad_heights=()):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        root = Path(tmp.name)
        calls, gates = [], []

        def runner(command, *, log_path, env):
            calls.append((command, env))
            height = next(h for h in heights if matrix.SCALE_BY_HEIGHT[h] in command[command.index("--output") + 1])
            out = Path(command[command.index("--output") + 1])
            if height in fail_heights:
                return {"exit_code": 137}
            out.write_text(json.dumps(fake_result(scheme, height, 8, correct=height not in bad_heights)))
            return {"exit_code": 0, "process_peak_rss_bytes": 1}

        code = matrix.main(["--root", str(root), "--scheme", scheme, "--heights", *map(str, heights), *extra],
                           runner=runner, gate=lambda cmd, check: gates.append(cmd))
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

    def test_treepir_spiral_reads_the_treepir_rows_formal_result(self):
        root, code, calls, _ = self.run_matrix([24, 27], scheme="treepir_spiral",
                                               extra=["--treepir-root", "/runs/treepir-backend-tuning"])
        self.assertEqual(code, 0)
        inputs = [command[command.index("--input") + 1] for command, _ in calls]
        self.assertEqual(inputs, ["/runs/treepir-backend-tuning/1gb/formal.json",
                                  "/runs/treepir-backend-tuning/8gb/formal.json"])
        self.assertTrue(all("--height" not in command for command, _ in calls))
        progress = json.loads((root / "metadata/treepir-spiral-progress.json").read_text())
        self.assertEqual(progress["path_semantics"], "legacy_colored_path_omits_root")
        self.assertTrue((root / "outputs/treepir-spiral/8gb/treepir_spiral.json").exists())

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

    def test_result_of_the_other_scheme_is_rejected(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        path = Path(tmp.name) / "r.json"
        path.write_text(json.dumps(fake_result("spiral_pbc", 24, 8)))
        with self.assertRaises(matrix.SpiralMatrixError):
            matrix.validate(path, scheme="treepir_spiral", height=24, trials=8, warmups=3, seed=7)

    def test_bad_arguments_are_rejected(self):
        for argv in (["--scheme", "spiral_pbc", "--heights", "21"],
                     ["--scheme", "treepir_spiral"],
                     ["--heights", "24"]):
            with self.subTest(argv=argv), self.assertRaises(SystemExit):
                matrix.parse_args(["--root", "/tmp/x", *argv])


if __name__ == "__main__":
    unittest.main()
