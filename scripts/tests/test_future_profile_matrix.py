from __future__ import annotations

import json
import os
import pathlib
import stat
import subprocess
import tempfile
import textwrap
import unittest


REPO = pathlib.Path(__file__).resolve().parents[2]
SCRIPT = REPO / "scripts" / "run_cloudlab_profile_matrix.sh"
LEAF_COUNT_1GIB = 1 << 24


def write_executable(path: pathlib.Path, body: str) -> None:
    path.parent.mkdir(parents=True, exist_ok=True)
    path.write_text(textwrap.dedent(body).lstrip())
    path.chmod(path.stat().st_mode | stat.S_IXUSR)


def arg_after(args: list[str], flag: str) -> str | None:
    try:
        return args[args.index(flag) + 1]
    except ValueError:
        return None


class FutureProfileMatrixTests(unittest.TestCase):
    def setUp(self) -> None:
        self.tmp = tempfile.TemporaryDirectory()
        self.addCleanup(self.tmp.cleanup)
        self.run_root = pathlib.Path(self.tmp.name)
        self.source = self.run_root / "source"
        self.fake_bin = self.run_root / "fake-bin"
        (self.run_root / "metadata").mkdir(parents=True)
        (self.run_root / "metadata" / "source-commit.txt").write_text("fake-commit\n")
        self.install_fakes()

    def install_fakes(self) -> None:
        scripts = self.source / "scripts"
        write_executable(
            scripts / "read_process_peak_memory.py",
            r"""
            #!/usr/bin/env python3
            import json
            import os
            import pathlib
            import subprocess
            import sys

            args = sys.argv[1:]
            output = pathlib.Path(args[args.index("--output") + 1])
            command = args[args.index("--") + 1:]
            root = pathlib.Path(os.environ["FAKE_RUN_ROOT"])
            with (root / "metadata" / "process-invocations.jsonl").open("a") as f:
                f.write(json.dumps({"output": str(output), "command": command}) + "\n")
            result = subprocess.run(command, env=os.environ.copy())
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_text(json.dumps({"exit_code": result.returncode}) + "\n")
            sys.exit(result.returncode)
            """,
        )
        write_executable(
            scripts / "summarize_cloudlab_profiles.py",
            r"""
            #!/usr/bin/env python3
            import json
            import pathlib
            import sys

            args = sys.argv[1:]
            if "--directory" in args:
                sys.exit(0)
            data = json.loads(pathlib.Path(args[args.index("--validate") + 1]).read_text())
            expected_trials = int(args[args.index("--trials") + 1])
            expected_leaves = int(args[args.index("--leaf-count") + 1])
            trials = data.get("measured_trials", data.get("trials"))
            leaves = data.get("leaf_count")
            sys.exit(0 if trials == expected_trials and leaves == expected_leaves else 1)
            """,
        )
        write_executable(
            scripts / "select_cuckoo_layout.py",
            r"""
            #!/usr/bin/env python3
            import json
            import pathlib
            import sys

            args = sys.argv[1:]
            output = pathlib.Path(args[args.index("--output") + 1])
            expected_trials = int(args[args.index("--expected-trials") + 1])
            expected_warmups = int(args[args.index("--expected-warmups") + 1])
            expected_leaves = int(args[args.index("--leaf-count") + 1])
            candidates = [
                pathlib.Path(a)
                for i, a in enumerate(args)
                if a.endswith(".json") and i > 0 and args[i - 1] not in {"--output"}
            ]
            for candidate in candidates:
                data = json.loads(candidate.read_text())
                if data["measured_trials"] != expected_trials or data["warmups"] != expected_warmups or data["leaf_count"] != expected_leaves:
                    sys.exit(1)
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_text(json.dumps({"chosen_height": 7}) + "\n")
            print("7")
            """,
        )
        write_executable(
            self.source / "build-native" / "Onion-PIR",
            r"""
            #!/usr/bin/env python3
            import json
            import os
            import pathlib
            import sys

            args = sys.argv[1:]

            def after(flag):
                return args[args.index(flag) + 1] if flag in args else None

            leaf_count = int(after("--leaf-count"))
            warmups = int(after("--warmup"))
            trials = int(after("--experiments"))
            if os.environ.get("SUPERRECORD_PLAN_ONLY") == "1":
                output = pathlib.Path(os.environ["SUPERRECORD_OUTPUT_JSON"])
                output.parent.mkdir(parents=True, exist_ok=True)
                output.write_text(json.dumps({
                    "leaf_schedule": {
                        "warmup": [1000 + i for i in range(warmups)],
                        "measured": [2000 + i for i in range(trials)],
                    }
                }) + "\n")
                sys.exit(0)

            output_name = (
                os.environ.get("SUPERRECORD_OUTPUT_JSON")
                or os.environ.get("BENCHMARK_OUTPUT_JSON")
                or after("--benchmark-json")
                or after("--layout-profile-json")
            )
            output = pathlib.Path(output_name)
            output.parent.mkdir(parents=True, exist_ok=True)
            if "--layout-profile-json" in args:
                output.write_text(json.dumps({
                    "measured_trials": trials,
                    "warmups": warmups,
                    "tree_height": leaf_count.bit_length() - 1,
                    "measurements": [{"height": 1}],
                }) + "\n")
            else:
                output.write_text(json.dumps({
                    "measured_trials": trials,
                    "warmups": warmups,
                    "leaf_count": leaf_count,
                }) + "\n")
            """,
        )
        write_executable(
            self.run_root / "deps" / "TreePIR" / "VBPIR_TreePIR" / "build" / "bin" / "vbpir_treepir",
            r"""
            #!/usr/bin/env python3
            import json
            import pathlib
            import sys

            args = sys.argv[1:]
            height = int(args[args.index("--height") + 1])
            warmups = int(args[args.index("--warmup") + 1])
            trials = int(args[args.index("--trials") + 1])
            output = pathlib.Path(args[args.index("--output") + 1])
            output.parent.mkdir(parents=True, exist_ok=True)
            output.write_text(json.dumps({
                "model": "SEAL+VBPIRTreePIR",
                "trials": trials,
                "warmups": warmups,
                "leaf_count": 1 << height,
            }) + "\n")
            """,
        )
        for name, output in {
            "lscpu": "Model name: Fake CPU\n",
            "g++": "g++ fake\n",
            "cmake": "cmake fake\n",
        }.items():
            write_executable(self.fake_bin / name, f"#!/usr/bin/env python3\nprint({output!r}, end='')\n")
        write_executable(
            self.fake_bin / "numactl",
            r"""
            #!/usr/bin/env python3
            import os
            import sys

            args = sys.argv[1:]
            while args and args[0].startswith("--"):
                args = args[1:]
            os.execvpe(args[0], args, os.environ.copy())
            """,
        )

    def run_matrix(self) -> subprocess.CompletedProcess[str]:
        env = os.environ.copy()
        env["FAKE_RUN_ROOT"] = str(self.run_root)
        env["PATH"] = f"{self.fake_bin}{os.pathsep}{env['PATH']}"
        return subprocess.run(
            ["bash", str(SCRIPT), str(self.run_root)],
            cwd=REPO,
            env=env,
            text=True,
            capture_output=True,
            check=False,
        )

    def invocations(self) -> list[dict[str, object]]:
        path = self.run_root / "metadata" / "process-invocations.jsonl"
        return [json.loads(line) for line in path.read_text().splitlines()]

    def formal_invocations(self) -> dict[str, list[str]]:
        formal = {}
        for invocation in self.invocations():
            output = pathlib.Path(invocation["output"])
            if output.parent.name == "1gb" and output.name.endswith("-process.json"):
                formal[output.name.removesuffix("-process.json")] = invocation["command"]
        return formal

    def test_launches_only_future_1gib_formal_cases_with_case_specific_trial_counts(self) -> None:
        result = self.run_matrix()

        self.assertEqual(result.returncode, 0, result.stderr)
        self.assertFalse((self.run_root / "outputs" / "profiling" / "8gb").exists())
        formal = self.formal_invocations()
        self.assertEqual(
            sorted(formal),
            ["cuckoo_tuned", "flat", "generic", "layerwise_tuned", "tree_g32", "treepir"],
        )
        for name in ["generic", "layerwise_tuned", "tree_g32"]:
            self.assertEqual(arg_after(formal[name], "--leaf-count"), str(LEAF_COUNT_1GIB))
            self.assertEqual(arg_after(formal[name], "--experiments"), "100")
            self.assertEqual(arg_after(formal[name], "--warmup"), "3")
        for name in ["cuckoo_tuned", "flat"]:
            self.assertEqual(arg_after(formal[name], "--leaf-count"), str(LEAF_COUNT_1GIB))
            self.assertEqual(arg_after(formal[name], "--experiments"), "16")
            self.assertEqual(arg_after(formal[name], "--warmup"), "3")
        self.assertEqual(arg_after(formal["treepir"], "--height"), "24")
        self.assertEqual(arg_after(formal["treepir"], "--trials"), "16")
        self.assertEqual(arg_after(formal["treepir"], "--warmup"), "3")

        leaves = (self.run_root / "outputs" / "profiling" / "1gb" / "leaves.txt").read_text().splitlines()
        self.assertEqual(len(leaves), 19)
        self.assertEqual(leaves[:3], ["1000", "1001", "1002"])
        self.assertEqual(leaves[-1], "2015")

    def test_refuses_to_overwrite_completed_output_with_mismatched_trial_count(self) -> None:
        folder = self.run_root / "outputs" / "profiling" / "1gb"
        folder.mkdir(parents=True)
        (folder / "flat.json").write_text(json.dumps({
            "measured_trials": 100,
            "warmups": 3,
            "leaf_count": LEAF_COUNT_1GIB,
        }) + "\n")
        (folder / "flat-process.json").write_text(json.dumps({"exit_code": 0}) + "\n")

        result = self.run_matrix()

        self.assertNotEqual(result.returncode, 0)
        self.assertIn("Existing completed result does not match this run's sample policy", result.stderr)
        formal = self.formal_invocations()
        self.assertNotIn("flat", formal)
        self.assertEqual(json.loads((folder / "flat.json").read_text())["measured_trials"], 100)


if __name__ == "__main__":
    unittest.main()
