from __future__ import annotations

import importlib.util
import json
import pathlib
import tempfile
import unittest
from types import SimpleNamespace


SCRIPTS = pathlib.Path(__file__).resolve().parents[1]
MODULE = SCRIPTS / "collect_cloudlab_results.py"
spec = importlib.util.spec_from_file_location("collect_cloudlab_results", MODULE)
collector = importlib.util.module_from_spec(spec)
spec.loader.exec_module(collector)

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


if __name__ == "__main__":
    unittest.main()
