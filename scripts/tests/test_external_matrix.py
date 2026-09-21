import json
import pathlib
import tempfile
import unittest
from contextlib import redirect_stderr
from io import StringIO
from unittest.mock import patch

from scripts.run_external_matrix import CommandResult, MatrixRunner, main, parse_args


REPO = pathlib.Path(__file__).resolve().parents[2]
HEX = "0123456789abcdef0123456789abcdef01234567"


def prepared_root(tmp: pathlib.Path) -> pathlib.Path:
    root = tmp / "run"
    root.mkdir()
    (root / "source").symlink_to(REPO, target_is_directory=True)
    (root / "deps").mkdir()
    (root / "wait_idle.py").write_text("print('idle')\n", encoding="utf-8")
    hexl = root / "hexl" / "lib64" / "cmake" / "hexl-1.2.6"
    hexl.mkdir(parents=True)
    return root


class ExternalMatrixTests(unittest.TestCase):
    def test_failed_matrix_returns_nonzero_to_the_parent_queue(self):
        argv = ["--root", "/tmp/x", "--source-commit", HEX, "--hexl-root", "/tmp/hexl"]
        for state, expected in (
            ("complete", 0),
            ("planned", 0),
            ("complete_with_resource_skips", 0),
            ("complete_with_failures", 1),
        ):
            with self.subTest(state=state), patch.object(MatrixRunner, "run", return_value={"state": state}):
                self.assertEqual(main(argv), expected)

    def test_parse_rejects_non_finite_heights_and_bad_commit(self):
        with redirect_stderr(StringIO()):
            with self.assertRaises(SystemExit):
                parse_args(["--root", "/tmp/x", "--source-commit", "abc", "--hexl-root", "/tmp/hexl"])
            with self.assertRaises(SystemExit):
                parse_args(["--root", "/tmp/x", "--source-commit", HEX, "--hexl-root", "/tmp/hexl", "--heights", "8"])

    def test_dry_run_records_four_scale_spirit_rc_and_respire_plan(self):
        with tempfile.TemporaryDirectory(prefix="external-matrix-test.") as td:
            root = prepared_root(pathlib.Path(td))
            args = parse_args([
                "--root", str(root),
                "--source-commit", HEX,
                "--hexl-root", str(root / "hexl"),
                "--dry-run",
                "--skip-build",
            ])
            with patch.object(MatrixRunner, "available_memory_bytes", return_value=1 << 40):
                state = MatrixRunner(args).run()

            self.assertEqual(state["state"], "planned")
            self.assertEqual(set(state["formal"]), {
                "256mib:spirit_rc", "256mib:respire",
                "512mib:spirit_rc", "512mib:respire",
                "1gib:spirit_rc", "1gib:respire",
                "2gib:spirit_rc", "2gib:respire",
                "4gib:spirit_rc", "4gib:respire",
            })
            two_gib = state["formal"]["2gib:respire"]["command"]
            self.assertEqual(two_gib[two_gib.index("--height") + 1], "25")
            command = state["formal"]["512mib:spirit_rc"]["command"]
            self.assertEqual(command[command.index("--scheme") + 1], "spirit_rc")
            self.assertEqual(command[command.index("--height") + 1], "23")
            respire = state["formal"]["4gib:respire"]["command"]
            self.assertEqual(respire[respire.index("--height") + 1], "26")
            self.assertEqual(respire[respire.index("--trials") + 1], "50")

            progress = json.loads((root / "metadata" / "external-matrix" / "progress.json").read_text(encoding="utf-8"))
            idle_events = [event for event in progress["events"] if event["kind"] == "idle_gate"]
            self.assertTrue(idle_events)
            self.assertEqual(idle_events[0]["command"][-2:], ["--root", str(root.resolve())])

    def test_respire_h26_is_explicitly_skipped_when_database_exceeds_memory(self):
        with tempfile.TemporaryDirectory(prefix="external-matrix-test.") as td:
            root = prepared_root(pathlib.Path(td))
            args = parse_args([
                "--root", str(root),
                "--source-commit", HEX,
                "--hexl-root", str(root / "hexl"),
                "--modes", "respire",
                "--heights", "26",
                "--dry-run",
                "--skip-build",
            ])
            with patch.object(MatrixRunner, "available_memory_bytes", return_value=1):
                state = MatrixRunner(args).run()

            record = state["formal"]["4gib:respire"]
            self.assertEqual(record["state"], "skipped_resource")
            self.assertIn("Respire CuckooRespireImpl Database", record["resource_admission"]["basis"])
            self.assertGreater(record["resource_admission"]["required_database_bytes"], 1)
            self.assertEqual(state["state"], "planned")

    def test_failed_adapter_process_is_preserved_without_summary(self):
        calls = []

        def fake_runner(command, *, cwd, env):
            calls.append(command)
            return CommandResult(0, "", "")

        with tempfile.TemporaryDirectory(prefix="external-matrix-test.") as td:
            root = prepared_root(pathlib.Path(td))
            args = parse_args([
                "--root", str(root),
                "--source-commit", HEX,
                "--hexl-root", str(root / "hexl"),
                "--modes", "respire",
                "--skip-build",
                "--heights", "24",
            ])
            with patch.object(MatrixRunner, "available_memory_bytes", return_value=1 << 40):
                state = MatrixRunner(args, command_runner=fake_runner).run()

            self.assertEqual(state["state"], "complete_with_failures")
            self.assertEqual(state["smoke"]["respire"]["state"], "failed")
            self.assertIn("No such file", state["smoke"]["respire"]["error"])
            self.assertEqual(state["formal"]["1gib:respire"]["state"], "skipped")
            self.assertTrue(any(str(root / "wait_idle.py") in call for command in calls for call in command))


if __name__ == "__main__":
    unittest.main()
