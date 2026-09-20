import json
import pathlib
import tempfile
import unittest
from contextlib import redirect_stderr
from io import StringIO

from scripts.clemson_external_matrix import CommandResult, MatrixRunner, parse_args


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


class ClemsonExternalMatrixTests(unittest.TestCase):
    def test_parse_rejects_non_finite_heights_and_bad_commit(self):
        with redirect_stderr(StringIO()):
            with self.assertRaises(SystemExit):
                parse_args(["--root", "/tmp/x", "--source-commit", "abc", "--hexl-root", "/tmp/hexl"])
            with self.assertRaises(SystemExit):
                parse_args(["--root", "/tmp/x", "--source-commit", HEX, "--hexl-root", "/tmp/hexl", "--heights", "8"])

    def test_dry_run_records_h24_h26_plan_and_managed_treepir_sealpir(self):
        with tempfile.TemporaryDirectory(prefix="clemson-external-test.") as td:
            root = prepared_root(pathlib.Path(td))
            args = parse_args([
                "--root", str(root),
                "--source-commit", HEX,
                "--hexl-root", str(root / "hexl"),
                "--modes", "respire", "treepir_spiral",
                "--dry-run",
                "--skip-build",
            ])
            state = MatrixRunner(args).run()

            self.assertEqual(state["state"], "planned")
            self.assertIn("1gib:respire", state["formal"])
            self.assertIn("4gib:respire", state["formal"])
            self.assertEqual(state["formal"]["1gib:respire"]["state"], "planned")
            self.assertEqual(state["formal"]["4gib:treepir_spiral"]["trials"], 8)
            self.assertEqual(state["gaps"]["treepir_sealpir"]["state"], "managed_separately")
            self.assertEqual(state["gaps"]["treepir_sealpir"]["path_semantics"], "legacy_colored_path_omits_root")
            self.assertEqual(
                state["gaps"]["treepir_sealpir"]["progress_json"],
                str(root.resolve() / "metadata" / "treepir-progress.json"),
            )
            self.assertIn("not as a canonical root-included six-mode", state["gaps"]["treepir_sealpir"]["reason"])

            progress = json.loads((root / "metadata" / "external-matrix" / "progress.json").read_text(encoding="utf-8"))
            idle_events = [event for event in progress["events"] if event["kind"] == "idle_gate"]
            self.assertTrue(idle_events)
            self.assertEqual(idle_events[0]["command"][-2:], ["--root", str(root.resolve())])

    def test_default_command_contract_counts_and_adapter_flags(self):
        with tempfile.TemporaryDirectory(prefix="clemson-external-test.") as td:
            root = prepared_root(pathlib.Path(td))
            args = parse_args([
                "--root", str(root),
                "--source-commit", HEX,
                "--hexl-root", str(root / "hexl"),
                "--modes", "spirit", "vbpir_pbc", "respire", "treepir_spiral",
                "--dry-run",
                "--skip-build",
            ])
            state = MatrixRunner(args).run()

            spirit = state["formal"]["1gib:spirit"]["command"]
            self.assertIn("--scheme", spirit)
            self.assertIn("spirit", spirit)
            self.assertEqual(spirit[spirit.index("--warmup") + 1], "3")
            self.assertEqual(spirit[spirit.index("--trials") + 1], "50")

            respire = state["formal"]["4gib:respire"]["command"]
            self.assertEqual(respire[respire.index("--height") + 1], "26")
            self.assertEqual(respire[respire.index("--trials") + 1], "50")

            spiral = state["formal"]["4gib:treepir_spiral"]["command"]
            self.assertEqual(spiral[spiral.index("--height") + 1], "26")
            self.assertEqual(spiral[spiral.index("--trials") + 1], "8")
            self.assertIn("--spiral-bin", spiral)
            self.assertIn("/h26/", spiral[spiral.index("--spiral-bin") + 1])

            vbpir_pilot = state["vbpir_pilots"]["h24"][0]["command"]
            self.assertEqual(vbpir_pilot[vbpir_pilot.index("--warmup") + 1], "3")
            self.assertEqual(vbpir_pilot[vbpir_pilot.index("--trials") + 1], "8")
            self.assertEqual(vbpir_pilot[vbpir_pilot.index("--first-dim") + 1], "256")

    def test_spiral_h26_build_stage_uses_manifest_helper(self):
        calls = []

        def fake_runner(command, *, cwd, env):
            calls.append(command)
            return CommandResult(0, "", "")

        with tempfile.TemporaryDirectory(prefix="clemson-external-test.") as td:
            root = prepared_root(pathlib.Path(td))
            args = parse_args([
                "--root", str(root),
                "--source-commit", HEX,
                "--hexl-root", str(root / "hexl"),
                "--modes", "treepir_spiral",
            ])
            runner = MatrixRunner(args, command_runner=fake_runner)
            ok, binary = runner.build_spiral_height(26)

            self.assertTrue(ok)
            self.assertIn("/h26/", str(binary))
            build_commands = [command for command in calls if "build_spiral_cpu.py" in " ".join(command)]
            self.assertEqual(len(build_commands), 1)
            self.assertEqual(build_commands[0][build_commands[0].index("--height") + 1], "26")
            self.assertEqual(build_commands[0][build_commands[0].index("--upstream-spiral") + 1], str(root.resolve() / "deps" / "TreePIR" / "Spiral"))

    def test_inspire_build_uses_poulpy_nightly_and_root_cargo_homes(self):
        captured_env = {}

        def fake_runner(command, *, cwd, env):
            captured_env.update(env)
            return CommandResult(0, "", "")

        with tempfile.TemporaryDirectory(prefix="clemson-external-test.") as td:
            root = prepared_root(pathlib.Path(td))
            (root / "deps" / "poulpy-pir").mkdir()
            args = parse_args([
                "--root", str(root),
                "--source-commit", HEX,
                "--hexl-root", str(root / "hexl"),
                "--modes", "inspire_layerwise",
            ])
            runner = MatrixRunner(args, command_runner=fake_runner)
            runner.ensure_inspire_stage = lambda: {"state": "ready", "method": "test"}
            ok, binary = runner.build_inspire()

            self.assertTrue(ok)
            self.assertIn("target-external", str(binary))
            self.assertEqual(captured_env["RUSTUP_TOOLCHAIN"], "nightly-2026-05-14")
            self.assertEqual(captured_env["CARGO_HOME"], str(root.resolve() / "deps" / "cargo-home"))
            self.assertEqual(captured_env["RUSTUP_HOME"], str(root.resolve() / "deps" / "rustup-home"))
            self.assertTrue(captured_env["PATH"].startswith(str(root.resolve() / "deps" / "cargo-home" / "bin")))
            self.assertEqual(captured_env["INSPIRE_PIR_FEATURES"], "avx512-fhe")

    def test_failed_adapter_process_is_preserved_without_summary(self):
        calls = []

        def fake_runner(command, *, cwd, env):
            calls.append(command)
            return CommandResult(0, "", "")

        with tempfile.TemporaryDirectory(prefix="clemson-external-test.") as td:
            root = prepared_root(pathlib.Path(td))
            args = parse_args([
                "--root", str(root),
                "--source-commit", HEX,
                "--hexl-root", str(root / "hexl"),
                "--modes", "respire",
                "--skip-build",
                "--heights", "24",
            ])
            state = MatrixRunner(args, command_runner=fake_runner).run()

            self.assertEqual(state["state"], "complete_with_failures")
            self.assertEqual(state["smoke"]["respire"]["state"], "failed")
            self.assertIn("No such file", state["smoke"]["respire"]["error"])
            self.assertEqual(state["formal"]["1gib:respire"]["state"], "skipped")
            self.assertTrue(any(str(root / "wait_idle.py") in call for command in calls for call in command))


if __name__ == "__main__":
    unittest.main()
