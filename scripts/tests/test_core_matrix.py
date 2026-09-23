from __future__ import annotations

import json
import pathlib
import sys
import tempfile
import unittest
from subprocess import CompletedProcess


sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import run_core_matrix as matrix


def ok(args: list[str], stdout: str = "") -> CompletedProcess[str]:
    return CompletedProcess(args=args, returncode=0, stdout=stdout, stderr="")


def fail(args: list[str], stderr: str = "failed") -> CompletedProcess[str]:
    return CompletedProcess(args=args, returncode=1, stdout="", stderr=stderr)


class FakeRunner(matrix.Runner):
    def __init__(self, *, fail_layer_profile_scales: set[str] | None = None) -> None:
        self.commands: list[list[str]] = []
        self.waits = 0
        self.fail_layer_profile_scales = fail_layer_profile_scales or set()

    def run(self, command: list[str], *, env: dict[str, str] | None = None,
            cwd: pathlib.Path | None = None) -> CompletedProcess[str]:
        self.commands.append(command)
        if command[1].endswith("wait_idle.py"):
            self.waits += 1
            return ok(command)
        if command[1].endswith("summarize_cloudlab_profiles.py"):
            data = json.loads(pathlib.Path(command[command.index("--validate") + 1]).read_text())
            expected_trials = int(command[command.index("--trials") + 1])
            expected_leaves = int(command[command.index("--leaf-count") + 1])
            return ok(command) if data.get("measured_trials") == expected_trials and data.get("leaf_count") == expected_leaves else fail(command)
        if command[1].endswith("select_cuckoo_layout.py"):
            output = pathlib.Path(command[command.index("--output") + 1])
            output.parent.mkdir(parents=True, exist_ok=True)
            height = int(command[command.index("--heights") + 1].split(",")[-1])
            output.write_text(json.dumps({"decision": {"chosen_expansion_height": height}}) + "\n")
            return ok(command, str(height) + "\n")
        if command[1].endswith("read_process_peak_memory.py"):
            return self.run_wrapper(command, env or {})
        raise AssertionError(f"unexpected command: {command}")

    def run_wrapper(self, command: list[str], env: dict[str, str]) -> CompletedProcess[str]:
        output = pathlib.Path(command[command.index("--output") + 1])
        child = command[command.index("--") + 1:]
        output.parent.mkdir(parents=True, exist_ok=True)
        output.write_text(json.dumps({"exit_code": 0, "process_peak_rss_bytes": 1234}) + "\n")
        if "--layout-profile-json" in child:
            profile = pathlib.Path(child[child.index("--layout-profile-json") + 1])
            scale = profile.parents[2].name
            if scale in self.fail_layer_profile_scales:
                output.write_text(json.dumps({"exit_code": 2}) + "\n")
                return CompletedProcess(args=command, returncode=2, stdout="", stderr="profile failed")
            profile.parent.mkdir(parents=True, exist_ok=True)
            leaf_count = int(child[child.index("--leaf-count") + 1])
            profile.write_text(json.dumps({
                "measured_trials": int(child[child.index("--experiments") + 1]),
                "warmups": int(child[child.index("--warmup") + 1]),
                "tree_height": leaf_count.bit_length() - 1,
                "measurements": [{"level": 0}],
            }) + "\n")
            return ok(command)
        json_path = pathlib.Path(env.get("BENCHMARK_OUTPUT_JSON") or child[child.index("--benchmark-json") + 1])
        json_path.parent.mkdir(parents=True, exist_ok=True)
        json_path.write_text(json.dumps({
            "scheme": "sampled" if "--benchmark-json" not in child else "merkle",
            "leaf_count": int(child[child.index("--leaf-count") + 1]),
            "warmups": int(child[child.index("--warmup") + 1]),
            "measured_trials": int(child[child.index("--experiments") + 1]),
            "samples": [
                {
                    "leaf": idx,
                    "correct": True,
                    "server_total_ms": 1.0,
                    "client_query_ms": 1.0,
                    "response_serialize_ms": 1.0,
                    "client_decode_ms": 1.0,
                    "stages_ms": {"scan": 1.0},
                }
                for idx in range(int(child[child.index("--experiments") + 1]))
            ],
        }) + "\n")
        return ok(command)


def make_paths(root: pathlib.Path) -> matrix.Paths:
    source = root / "source"
    for script in ("read_process_peak_memory.py", "summarize_cloudlab_profiles.py", "select_cuckoo_layout.py"):
        (source / "scripts" / script).parent.mkdir(parents=True, exist_ok=True)
        (source / "scripts" / script).write_text("# fake\n")
    (root / "wait_idle.py").write_text("# fake\n")
    return matrix.make_paths(root, root / "outputs" / "core-matrix")


def make_standard_source(root: pathlib.Path) -> pathlib.Path:
    source = root / "standard-source"
    source.mkdir(parents=True)
    (source / "standard-process.json").write_text(json.dumps({"exit_code": 0}) + "\n")
    (source / "standard.json").write_text(json.dumps({
        "leaf_count": matrix.SCALES["1gb"],
        "measured_trials": matrix.STANDARD_TRIALS,
        "warmups": matrix.WARMUPS,
    }) + "\n")
    return source


class CoreMatrixTests(unittest.TestCase):
    def run_matrix(self, runner: FakeRunner, *, reuse_standard: bool = True) -> tuple[pathlib.Path, int]:
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        root = pathlib.Path(tmp.name)
        paths = make_paths(root)
        source = make_standard_source(root) if reuse_standard else None
        result = matrix.CoreMatrix(paths, standard_1g_source=source, runner=runner).run()
        return root, result

    def test_full_matrix_uses_wait_gate_and_requested_counts_without_generic_cases(self) -> None:
        runner = FakeRunner()
        root, result = self.run_matrix(runner)

        self.assertEqual(result, 0)
        self.assertEqual(runner.waits, 38)
        status = json.loads((root / "outputs" / "core-matrix" / "status.json").read_text())
        self.assertEqual(status["state"], "complete")
        labels = [(row["scale"], row["case"], row["status"]) for row in status["rows"]]
        self.assertIn(("256mb", "standard", "complete"), labels)
        self.assertIn(("512mb", "standard", "complete"), labels)
        self.assertIn(("1gb", "standard_reused", "complete"), labels)
        self.assertIn(("2gb", "standard", "complete"), labels)
        self.assertIn(("4gb", "standard", "complete"), labels)
        two_gb = [row for row in status["rows"] if row["scale"] == "2gb"]
        self.assertTrue(all(row["status"] == "complete" for row in two_gb))
        self.assertEqual({row["case"] for row in two_gb if row["case"].startswith("tuning/cuckoo/h")},
                         {"tuning/cuckoo/h7", "tuning/cuckoo/h8", "tuning/cuckoo/h9", "tuning/cuckoo/h10"})
        self.assertTrue(all("--reference-expansion-height" not in command for command in runner.commands))
        self.assertNotIn("generic", {row["case"] for row in status["rows"]})
        self.assertNotIn("tree_g32", {row["case"] for row in status["rows"]})
        self.assertFalse(any("tree_superrecord_bench" in command for command in runner.commands for command in command))
        self.assertFalse(any("tree_bench_g32" in command for command in runner.commands for command in command))
        wrappers = [command for command in runner.commands if command[1].endswith("read_process_peak_memory.py")]
        self.assertTrue(all("--physcpubind=0" in command and "--preferred=0" in command for command in wrappers))
        formal_trials = {
            row["case"]: row["trials"]
            for row in status["rows"]
            if row["scale"] == "4gb" and row["case"] in {"standard", "layerwise_tuned", "flat", "cuckoo_tuned"}
        }
        self.assertEqual(formal_trials, {
            "standard": 50,
            "layerwise_tuned": 50,
            "flat": 8,
            "cuckoo_tuned": 8,
        })

    def test_small_scales_do_not_request_heights_outside_reference_envelope(self):
        runner = FakeRunner()
        root, result = self.run_matrix(runner)
        self.assertEqual(result, 0)
        status = json.loads((root / "outputs/core-matrix/status.json").read_text())
        for scale in ("256mb", "512mb"):
            pilots = [row["case"] for row in status["rows"]
                      if row["scale"] == scale and row["case"].startswith("tuning/cuckoo/h")]
            self.assertEqual(pilots, ["tuning/cuckoo/h7"])
            formal = [row for row in status["rows"] if row["scale"] == scale and row["case"] == "cuckoo_tuned"]
            self.assertEqual(len(formal), 1)
            self.assertEqual(formal[0]["status"], "complete")
            self.assertEqual(formal[0]["trials"], 8)

    def test_standard_1g_is_validated_and_copied_without_waiting(self) -> None:
        runner = FakeRunner()
        root, _ = self.run_matrix(runner)

        copied = root / "outputs" / "core-matrix" / "1gb" / "standard.json"
        self.assertTrue(copied.exists())
        status = json.loads((root / "outputs" / "core-matrix" / "status.json").read_text())
        row = next(row for row in status["rows"] if row["case"] == "standard_reused")
        self.assertEqual(row["status"], "complete")
        self.assertNotIn("wait_idle", row)

    def test_default_runs_standard_1gb_fresh_when_reuse_source_is_not_explicit(self) -> None:
        runner = FakeRunner()
        root, result = self.run_matrix(runner, reuse_standard=False)

        self.assertEqual(result, 0)
        status = json.loads((root / "outputs" / "core-matrix" / "status.json").read_text())
        rows = {(row["scale"], row["case"]): row for row in status["rows"]}
        self.assertIn(("1gb", "standard"), rows)
        self.assertNotIn(("1gb", "standard_reused"), rows)
        self.assertEqual(rows[("1gb", "standard")]["status"], "complete")

    def test_path_semantics_are_recorded_per_case(self) -> None:
        runner = FakeRunner()
        root, _ = self.run_matrix(runner)

        status = json.loads((root / "outputs" / "core-matrix" / "status.json").read_text())
        rows = {(row["scale"], row["case"]): row for row in status["rows"]}
        self.assertEqual(rows[("1gb", "standard_reused")]["path_semantics"], "single_plaintext_block")
        self.assertEqual(rows[("4gb", "standard")]["path_semantics"], "single_plaintext_block")
        self.assertEqual(rows[("1gb", "layerwise_tuned")]["path_semantics"], "legacy_merkle_paths_omit_root")
        self.assertEqual(rows[("1gb", "tuning/cuckoo/h7")]["path_semantics"], "legacy_merkle_paths_omit_root")
        self.assertEqual(rows[("1gb", "cuckoo_tuned")]["path_semantics"], "legacy_merkle_paths_omit_root")
        flat = rows[("1gb", "flat")]
        self.assertEqual(flat["path_semantics"], "legacy_merkle_paths_omit_root")
        self.assertEqual(flat["primary_case"], "merkle_flat")
        self.assertEqual(flat["cli_case_selection"], "merkle_paths")
        self.assertIn("merkle_layerwise", flat["cli_case_note"])

    def test_scale_and_case_filters_with_pinned_geometry_measure_instead_of_reusing(self) -> None:
        runner = FakeRunner()
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        root = pathlib.Path(tmp.name)
        paths = make_paths(root)
        source = make_standard_source(root)
        result = matrix.CoreMatrix(
            paths,
            standard_1g_source=source,
            runner=runner,
            scales=("256mb", "512mb", "1gb"),
            cases=("standard",),
            reference_expansion_height=10,
        ).run()

        self.assertEqual(result, 0)
        status = json.loads((root / "outputs/core-matrix/status.json").read_text())
        self.assertEqual(status["scales"], ["256mb", "512mb", "1gb"])
        self.assertEqual(status["cases"], ["standard"])
        self.assertEqual(status["reference_expansion_height"], 10)
        self.assertEqual([(row["scale"], row["case"]) for row in status["rows"]],
                         [("256mb", "standard"), ("512mb", "standard"), ("1gb", "standard")])
        self.assertTrue(all(row["status"] == "complete" and row["reference_expansion_height"] == 10
                            for row in status["rows"]))
        children = [command[command.index("--") + 1:] for command in runner.commands
                    if command[1].endswith("read_process_peak_memory.py")]
        self.assertEqual(len(children), 3)
        for child in children:
            self.assertEqual(child[child.index("--reference-expansion-height") + 1], "10")
            self.assertEqual(child[child.index("--benchmark-case") + 1], "standard_onionpir")
        self.assertEqual(runner.waits, 3)

    def test_pinned_geometry_reaches_layerwise_cuckoo_flat_and_widens_small_cuckoo_pilots(self) -> None:
        runner = FakeRunner()
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        root = pathlib.Path(tmp.name)
        paths = make_paths(root)
        result = matrix.CoreMatrix(
            paths,
            runner=runner,
            scales=("256mb",),
            cases=("layerwise", "layerwise_merged", "cuckoo", "flat"),
            reference_expansion_height=10,
        ).run()
        self.assertEqual(result, 0)
        status = json.loads((root / "outputs/core-matrix/status.json").read_text())
        cases = [row["case"] for row in status["rows"]]
        self.assertEqual(cases, [
            "tuning/layerwise/profile", "layerwise_tuned",
            "tuning/layerwise_merged/profile", "layerwise_merged_tuned",
            "tuning/cuckoo/h7", "tuning/cuckoo/h8", "tuning/cuckoo/h9", "tuning/cuckoo/h10",
            "tuning/cuckoo/selection", "cuckoo_tuned", "flat",
        ])
        self.assertTrue(all(row["status"] == "complete" for row in status["rows"]))
        children = [command[command.index("--") + 1:] for command in runner.commands
                    if command[1].endswith("read_process_peak_memory.py")]
        self.assertEqual(len(children), 10)
        for child in children:
            self.assertEqual(child[child.index("--reference-expansion-height") + 1], "10")
        merged_profile = [c for c in children if "--layout-profile-json" in c and "layerwise_merged" in c[c.index("--layout-profile-json") + 1]]
        self.assertEqual(len(merged_profile), 1)
        self.assertEqual(merged_profile[0][merged_profile[0].index("--layer-nodes-per-plaintext") + 1], "64")
        plain_profile = [c for c in children if "--layout-profile-json" in c and "layerwise_merged" not in c[c.index("--layout-profile-json") + 1]]
        self.assertNotIn("--layer-nodes-per-plaintext", plain_profile[0])
        merged_formal = [c for c in children if "--benchmark-case" in c and c[c.index("--benchmark-case") + 1] == "merkle_layerwise_merged"]
        self.assertEqual(len(merged_formal), 1)
        self.assertTrue(merged_formal[0][merged_formal[0].index("--layer-layout-profile") + 1].endswith("tuning/layerwise_merged/profile.json"))

    def test_main_rejects_unknown_scale_and_out_of_range_geometry(self) -> None:
        with self.assertRaises(SystemExit):
            matrix.parse_args(["--root", "/tmp/x", "--scales", "3gb"])
        with self.assertRaises(SystemExit):
            matrix.parse_args(["--root", "/tmp/x", "--reference-expansion-height", "11"])
        args = matrix.parse_args(["--root", "/tmp/x", "--scales", "2gb", "--cases", "flat", "standard"])
        self.assertEqual(args.scales, ["2gb"])
        self.assertNotIn("8gb", matrix.parse_args(["--root", "/tmp/x"]).scales)
        self.assertEqual(matrix.parse_args(["--root", "/tmp/x", "--scales", "8gb"]).scales, ["8gb"])
        self.assertEqual(matrix.SCALES["8gb"], 1 << 27)
        self.assertEqual(sorted(args.cases), ["flat", "standard"])

    def test_failed_layer_profile_skips_layerwise_formal_but_continues_independent_cases(self) -> None:
        runner = FakeRunner(fail_layer_profile_scales={"1gb"})
        root, result = self.run_matrix(runner)

        self.assertEqual(result, 1)
        status = json.loads((root / "outputs" / "core-matrix" / "status.json").read_text())
        rows = {(row["scale"], row["case"]): row for row in status["rows"]}
        self.assertEqual(rows[("1gb", "tuning/layerwise/profile")]["status"], "failed")
        self.assertEqual(rows[("1gb", "layerwise_tuned")]["status"], "skipped")
        self.assertEqual(rows[("1gb", "flat")]["status"], "complete")
        self.assertEqual(rows[("4gb", "layerwise_tuned")]["status"], "complete")


if __name__ == "__main__":
    unittest.main()
