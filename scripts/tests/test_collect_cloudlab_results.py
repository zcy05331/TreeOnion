from __future__ import annotations

import ast
import importlib.util
import json
import pathlib
import re
import sys
import tempfile
import unittest
from types import SimpleNamespace


SCRIPTS = pathlib.Path(__file__).resolve().parents[1]
MODULE = SCRIPTS / "collect_cloudlab_results.py"
spec = importlib.util.spec_from_file_location("collect_cloudlab_results", MODULE)
collector = importlib.util.module_from_spec(spec)
spec.loader.exec_module(collector)

sys.path.insert(0, str(SCRIPTS))
import run_wisconsin_alpha_entries as controller

CONTROLLER_PY = SCRIPTS / "run_wisconsin_alpha_entries.py"
CONTROLLER_SH = SCRIPTS / "run_wisconsin_alpha_entries.sh"


def assigned_strings(value: ast.expr) -> set[str]:
    """The string literals `value` can evaluate to (the controller picks its final status with `a if c else b`)."""
    if isinstance(value, ast.IfExp):
        return assigned_strings(value.body) | assigned_strings(value.orelse)
    if isinstance(value, ast.Constant) and isinstance(value.value, str):
        return {value.value}
    return set()


def controller_terminal_statuses() -> set[str]:
    """The statuses the Wisconsin controller and its bash wrapper write when they stop.

    Read out of the producers instead of retyped here, so renaming one of them (which would
    leave the collector polling a run that has already stopped) fails here, not on CloudLab.
    """
    main = next(node for node in ast.parse(CONTROLLER_PY.read_text()).body
                if isinstance(node, ast.FunctionDef) and node.name == "main")
    statuses: set[str] = set()
    for node in ast.walk(main):
        if isinstance(node, ast.Assign) and any(
                isinstance(target, ast.Subscript) and getattr(target.slice, "value", None) == "status"
                for target in node.targets):
            statuses |= assigned_strings(node.value)
    return statuses | set(re.findall(r"\['status'\]\s*=\s*'([a-z_]+)'", CONTROLLER_SH.read_text()))


SSH_OPTIONS = [
    "-o", "BatchMode=yes",
    "-o", "ConnectTimeout=10",
    "-o", "ServerAliveInterval=15",
    "-o", "ServerAliveCountMax=2",
]
RSYNC_PREFIX = ["rsync", "-az", "--no-perms", "-e", "ssh " + " ".join(SSH_OPTIONS)]


def is_progress_read(command: list[str]) -> bool:
    return command[:1] == ["ssh"] and command[1:1 + len(SSH_OPTIONS)] == SSH_OPTIONS and command[1 + len(SSH_OPTIONS):] == ["node", "cat", "/remote/run/metadata/progress.json"]


class CollectCloudlabResultsTests(unittest.TestCase):
    def test_copies_only_finished_stage_and_metadata_then_exits_on_terminal_status(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            local_root = pathlib.Path(tmp) / "local"
            commands: list[list[str]] = []
            progress = {
                "status": "complete_with_failures",
                "finished_stages": ["alpha", "entry16"],
                "failed_stages": ["not-finished"],
            }

            def fake_run(command: list[str], **kwargs) -> SimpleNamespace:
                commands.append(command)
                if is_progress_read(command):
                    return SimpleNamespace(returncode=0, stdout=json.dumps(progress), stderr="")
                return SimpleNamespace(returncode=0, stdout="", stderr="")

            code = collector.main([
                "--host", "node",
                "--remote-root", "/remote/run",
                "--local-root", str(local_root),
                "--interval", "0",
            ], runner=fake_run, sleeper=lambda _: None)

            self.assertEqual(code, 0)
            rsync_commands = [command for command in commands if command and command[0] == "rsync"]
            self.assertEqual(rsync_commands, [
                RSYNC_PREFIX + ["node:/remote/run/metadata/", str(local_root / "metadata") + "/"],
                RSYNC_PREFIX + ["node:/remote/run/outputs/alpha/", str(local_root / "outputs" / "alpha") + "/"],
                RSYNC_PREFIX + ["node:/remote/run/outputs/entry16/", str(local_root / "outputs" / "entry16") + "/"],
            ])
            collection = json.loads((local_root / "collection.json").read_text())
            self.assertEqual(collection["status"], "complete_with_failures")
            self.assertEqual(collection["copied_stages"], ["alpha", "entry16"])
            self.assertNotIn("not-finished", collection["copied_stages"])
            self.assertTrue(collection["terminal"])
            self.assertFalse(collection["unreachable"])

    def test_skips_stage_already_recorded_in_collection_json(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            local_root = pathlib.Path(tmp) / "local"
            local_root.mkdir()
            (local_root / "collection.json").write_text(json.dumps({
                "copied_stages": ["alpha"],
                "status": "running",
                "terminal": False,
                "unreachable": False,
            }) + "\n")
            commands: list[list[str]] = []
            progress = {"status": "complete", "finished_stages": ["alpha", "entry16"]}

            def fake_run(command: list[str], **kwargs) -> SimpleNamespace:
                commands.append(command)
                if is_progress_read(command):
                    return SimpleNamespace(returncode=0, stdout=json.dumps(progress), stderr="")
                return SimpleNamespace(returncode=0, stdout="", stderr="")

            code = collector.main([
                "--host", "node",
                "--remote-root", "/remote/run/",
                "--local-root", str(local_root),
                "--interval", "0",
            ], runner=fake_run, sleeper=lambda _: None)

            self.assertEqual(code, 0)
            rsync_sources = [command[-2] for command in commands if command and command[0] == "rsync"]
            self.assertEqual(rsync_sources, [
                "node:/remote/run/metadata/",
                "node:/remote/run/outputs/entry16/",
            ])
            collection = json.loads((local_root / "collection.json").read_text())
            self.assertEqual(collection["copied_stages"], ["alpha", "entry16"])

    def test_waits_when_progress_json_is_not_created_yet(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            local_root = pathlib.Path(tmp) / "local"
            commands: list[list[str]] = []
            sleeps: list[float] = []
            progress = {"status": "complete", "finished_stages": ["alpha"]}

            def fake_run(command: list[str], **kwargs) -> SimpleNamespace:
                commands.append(command)
                ssh_reads = [cmd for cmd in commands if is_progress_read(cmd)]
                if is_progress_read(command) and len(ssh_reads) == 1:
                    return SimpleNamespace(returncode=1, stdout="", stderr="cat: /remote/run/metadata/progress.json: No such file or directory")
                if is_progress_read(command):
                    return SimpleNamespace(returncode=0, stdout=json.dumps(progress), stderr="")
                return SimpleNamespace(returncode=0, stdout="", stderr="")

            code = collector.main([
                "--host", "node",
                "--remote-root", "/remote/run",
                "--local-root", str(local_root),
                "--interval", "0.25",
            ], runner=fake_run, sleeper=sleeps.append)

            self.assertEqual(code, 0)
            self.assertEqual(sleeps, [0.25])
            collection = json.loads((local_root / "collection.json").read_text())
            self.assertEqual(collection["status"], "complete")
            self.assertFalse(collection["unreachable"])

    def test_records_unreachable_after_three_consecutive_connection_failures(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            local_root = pathlib.Path(tmp) / "local"
            commands: list[list[str]] = []

            def fake_run(command: list[str], **kwargs) -> SimpleNamespace:
                commands.append(command)
                return SimpleNamespace(returncode=255, stdout="", stderr="ssh: connect to host node port 22: Connection refused")

            code = collector.main([
                "--host", "node",
                "--remote-root", "/remote/run",
                "--local-root", str(local_root),
                "--interval", "0",
            ], runner=fake_run, sleeper=lambda _: None)

            self.assertEqual(code, 2)
            self.assertEqual([command[0] for command in commands], ["ssh", "ssh", "ssh"])
            collection = json.loads((local_root / "collection.json").read_text())
            self.assertTrue(collection["unreachable"])
            self.assertFalse(collection["terminal"])
            self.assertIn("Connection refused", collection["last_error"])

    def test_stage_that_keeps_failing_does_not_lose_the_stages_already_copied(self) -> None:
        # An unreadable stage directory must cost that stage only: the finished stages already
        # on the laptop are not re-fetched on the retries, and the error names the stage.
        with tempfile.TemporaryDirectory() as tmp:
            local_root = pathlib.Path(tmp) / "local"
            commands: list[list[str]] = []
            progress = {"status": "complete", "finished_stages": ["alpha", "entry16"]}

            def fake_run(command: list[str], **kwargs) -> SimpleNamespace:
                commands.append(command)
                if is_progress_read(command):
                    return SimpleNamespace(returncode=0, stdout=json.dumps(progress), stderr="")
                if command[0] == "rsync" and command[-2].endswith("/outputs/entry16/"):
                    return SimpleNamespace(returncode=23, stdout="", stderr="rsync: permission denied")
                return SimpleNamespace(returncode=0, stdout="", stderr="")

            code = collector.main([
                "--host", "node",
                "--remote-root", "/remote/run",
                "--local-root", str(local_root),
                "--interval", "0",
            ], runner=fake_run, sleeper=lambda _: None)

            self.assertEqual(code, 2)
            collection = json.loads((local_root / "collection.json").read_text())
            self.assertEqual(collection["copied_stages"], ["alpha"])
            self.assertFalse(collection["terminal"])
            self.assertTrue(collection["unreachable"])
            self.assertIn("stage entry16 sync failed", collection["last_error"])
            self.assertIn("permission denied", collection["last_error"])
            rsync_sources = [command[-2] for command in commands if command[0] == "rsync"]
            self.assertEqual(rsync_sources.count("node:/remote/run/outputs/alpha/"), 1)
            self.assertEqual(rsync_sources.count("node:/remote/run/outputs/entry16/"), 3)

    def test_successful_poll_between_ssh_failures_resets_the_failure_count(self) -> None:
        # ssh flapping twice, recovering, then flapping twice again is not an unreachable node:
        # without the reset the fifth poll would abandon a run that finishes on the sixth.
        with tempfile.TemporaryDirectory() as tmp:
            local_root = pathlib.Path(tmp) / "local"
            refused = SimpleNamespace(returncode=255, stdout="", stderr="ssh: connect to host node port 22: Connection refused")
            responses = [
                refused,
                refused,
                SimpleNamespace(returncode=0, stdout=json.dumps({"status": "running", "finished_stages": []}), stderr=""),
                refused,
                refused,
                SimpleNamespace(returncode=0, stdout=json.dumps({"status": "complete", "finished_stages": ["alpha"]}), stderr=""),
            ]
            reads = iter(responses)

            def fake_run(command: list[str], **kwargs) -> SimpleNamespace:
                if is_progress_read(command):
                    return next(reads)
                return SimpleNamespace(returncode=0, stdout="", stderr="")

            code = collector.main([
                "--host", "node",
                "--remote-root", "/remote/run",
                "--local-root", str(local_root),
                "--interval", "0",
            ], runner=fake_run, sleeper=lambda _: None)

            self.assertEqual(code, 0)
            self.assertEqual(next(reads, None), None)
            collection = json.loads((local_root / "collection.json").read_text())
            self.assertTrue(collection["terminal"])
            self.assertFalse(collection["unreachable"])
            self.assertEqual(len(collection["errors"]), 4)
            self.assertEqual(collection["copied_stages"], ["alpha"])


class WisconsinProgressContractTests(unittest.TestCase):
    """The progress file the Wisconsin controller writes, read by the collector that polls it."""

    def test_collector_reads_the_progress_file_the_controller_writes(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp) / "run"
            (root / "metadata").mkdir(parents=True)
            state = {"status": "running", "finished_stages": [], "stages": {}}

            def failing_stage(folder: pathlib.Path) -> None:
                raise RuntimeError("driver exited 1")

            controller.run_stage(root, state, "alpha_ab", failing_stage)
            controller.run_stage(root, state, "entry16", lambda folder: {"ok": True})
            progress_text = (root / "metadata" / "progress.json").read_text()

            def fake_run(command: list[str], **kwargs) -> SimpleNamespace:
                if is_progress_read(command):
                    return SimpleNamespace(returncode=0, stdout=progress_text, stderr="")
                return SimpleNamespace(returncode=0, stdout="", stderr="")

            progress, error = collector.read_remote_progress("node", "/remote/run", fake_run)

            self.assertIsNone(error)
            self.assertEqual(progress["status"], "running")
            # A stage that failed still counts as finished, so its logs are collected too.
            self.assertEqual(state["stages"]["alpha_ab"]["status"], "failed")
            self.assertEqual(collector.stages_from_progress(progress), ["alpha_ab", "entry16"])

    def test_collector_treats_every_controller_stop_status_as_terminal(self) -> None:
        self.assertEqual(controller_terminal_statuses(), collector.TERMINAL_STATUSES)


if __name__ == "__main__":
    unittest.main()
