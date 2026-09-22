import json
from pathlib import Path
import sys
import tempfile
import unittest

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
import run_kspir_pbc_matrix as matrix

SPEED = {("gcc", 128): 12.0, ("icpx", 128): 10.0, ("icpx", 64): 9.0, ("icpx", 256): 11.0}


class KspirPbcMatrixTests(unittest.TestCase):
    def run_matrix(self, extra=(), fail=frozenset()):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        root = Path(tmp.name)
        calls = []

        def runner(command, *, log_path, env):
            label = command[command.index("--compiler-label") + 1]
            height = int(command[command.index("--height") + 1])
            n1 = int(command[command.index("--n1") + 1])
            trials = int(command[command.index("--trials") + 1])
            calls.append((label, height, n1, trials, command))
            if (label, n1) in fail:
                return {"exit_code": 134}
            out = Path(command[command.index("--output") + 1])
            out.write_text(json.dumps({
                "scheme": "kspir_pbc", "height": height, "measured_trials": trials, "warmups": 3, "trial_seed": 7,
                "kspir_parameters": {"bsgs_n1": n1},
                "samples": [{"correct": True} for _ in range(trials)],
                "measured_summary": {"all_correct": True, "server_ms_mean": SPEED.get((label, n1), 50.0)}}))
            return {"exit_code": 0}

        code = matrix.main(["--root", str(root), "--binary", "gcc=/b/gcc", "--binary", "icpx=/b/icpx", *extra],
                           runner=runner, gate=lambda cmd, check: None)
        return root, code, calls

    def test_faster_compiler_then_best_n1_per_height_then_formal(self):
        root, code, calls = self.run_matrix(["--heights", "22", "26"])
        self.assertEqual(code, 0)
        self.assertEqual([c[:4] for c in calls[:2]], [("gcc", 24, 128, 3), ("icpx", 24, 128, 3)])
        progress = json.loads((root / "metadata/kspir-pbc-progress.json").read_text())
        self.assertEqual(progress["chosen_compiler"], "icpx")
        for h in ("22", "26"):
            self.assertEqual(progress["heights"][h]["chosen_n1"], 64)
            self.assertEqual(progress["heights"][h]["state"], "complete")
        formal = [c for c in calls if c[3] == 8]
        self.assertEqual([(c[0], c[1], c[2]) for c in formal], [("icpx", 22, 64), ("icpx", 26, 64)])
        for c in calls:
            self.assertEqual(c[4][:3], ["numactl", "--physcpubind=0", "--preferred=0"])

    def test_failed_compiler_is_skipped_and_failed_n1_is_not_selected(self):
        root, code, calls = self.run_matrix(["--heights", "24"], fail={("icpx", 128), ("icpx", 64)})
        progress = json.loads((root / "metadata/kspir-pbc-progress.json").read_text())
        self.assertEqual(progress["chosen_compiler"], "gcc")
        self.assertEqual(progress["compiler_pilot"]["icpx"]["state"], "failed")
        self.assertEqual(code, 0)

    def test_bad_binary_spec_and_n1_are_rejected(self):
        with self.assertRaises(SystemExit):
            matrix.parse_args(["--root", "/x", "--binary", "gcc"])
        with self.assertRaises(SystemExit):
            matrix.parse_args(["--root", "/x", "--binary", "gcc=/b", "--n1-candidates", "96"])


if __name__ == "__main__":
    unittest.main()
