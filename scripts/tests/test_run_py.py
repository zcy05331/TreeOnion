"""run.py argv mapping: which flags reach the Onion-PIR binary, where a bare
--benchmark-json name is routed, and that a bad --leaf-count never launches
the binary. build() and subprocess are patched, so nothing is compiled or run
and the repository is never written to (OUTPUT_DIR points at a temp dir)."""
import collections
import contextlib
import io
import os
import pathlib
import sys
import tempfile
import types
import unittest
from unittest.mock import patch

RUN_PY = pathlib.Path(__file__).resolve().parents[2] / "run.py"
# run.py lives at the repository root, not under scripts/: execute its source
# into a private module (no sys.path entry, no bytecode cache in the root).
run = types.ModuleType("onionpir_run_py")
run.__file__ = str(RUN_PY)
exec(compile(RUN_PY.read_text(), str(RUN_PY), "exec"), run.__dict__)

Invocation = collections.namedtuple("Invocation", "exit_code events launches stderr")


class RunPyArgvMappingTest(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        self.outputs = tmp.name

    def invoke(self, *argv):
        """Run run.main() on argv with build() and the binary launch replaced by
        recorders. events lists 'build'/'launch' in call order; launches holds the
        recorded (argv, kwargs) of each subprocess.run call."""
        events = []
        launches = []

        def fake_build(build_type, jobs, active_config):
            events.append("build")

        def fake_launch(cmd, **kwargs):
            events.append("launch")
            launches.append((list(cmd), kwargs))

        stderr = io.StringIO()
        exit_code = None
        with patch.object(run, "build", side_effect=fake_build), \
                patch.object(run, "subprocess") as fake_subprocess, \
                patch.object(run, "OUTPUT_DIR", self.outputs), \
                patch.object(sys, "argv", ["run.py", *argv]), \
                contextlib.redirect_stdout(io.StringIO()), \
                contextlib.redirect_stderr(stderr):
            fake_subprocess.run.side_effect = fake_launch
            try:
                run.main()
            except SystemExit as exc:
                exit_code = exc.code
        return Invocation(exit_code, events, launches, stderr.getvalue())

    def merkle_prefix(self):
        return [run.BINARY, "--test", "merkle_benchmarks",
                "--experiments", "5", "--warmup", "3"]

    # run.py builds first and validates --leaf-count afterwards, so the build
    # step is recorded; what protects the user from a typo starting a 2^24-leaf
    # benchmark is that the binary never launches.
    def test_non_power_of_two_leaf_count_is_rejected_before_launch(self):
        result = self.invoke("-t", "merkle_benchmarks", "--leaf-count", "1000")
        self.assertEqual(result.exit_code, 2)
        self.assertIn("power of two", result.stderr)
        self.assertEqual(result.events, ["build"])
        self.assertEqual(result.launches, [])

    def test_leaf_count_above_cap_is_rejected_before_launch(self):
        result = self.invoke("-t", "merkle_benchmarks", "--leaf-count", str(1 << 25))
        self.assertEqual(result.exit_code, 2)
        self.assertIn("capped at 2^24", result.stderr)
        self.assertEqual(result.events, ["build"])
        self.assertEqual(result.launches, [])

    def test_merkle_flags_forwarded_and_bare_json_name_routed(self):
        result = self.invoke("-t", "merkle_benchmarks", "--leaf-count", "256",
                             "--benchmark-case", "merkle_paths",
                             "--benchmark-json", "foo.json")
        self.assertIsNone(result.exit_code)
        self.assertEqual(result.events, ["build", "launch"])
        routed = os.path.join(self.outputs, "merkle_baselines", "foo.json")
        (argv, _kwargs), = result.launches
        self.assertEqual(argv, self.merkle_prefix() + [
            "--leaf-count", "256",
            "--benchmark-case", "merkle_paths",
            "--benchmark-json", routed])
        # The archive directory must exist before the binary tries to write.
        self.assertTrue(os.path.isdir(os.path.dirname(routed)))

    def test_json_path_with_directory_is_forwarded_verbatim(self):
        target = os.path.join(self.outputs, "archive", "run.json")
        result = self.invoke("-t", "merkle_benchmarks", "--leaf-count", "256",
                             "--benchmark-json", target)
        self.assertEqual(result.events, ["build", "launch"])
        (argv, _kwargs), = result.launches
        self.assertEqual(argv[argv.index("--benchmark-json") + 1], target)
        self.assertTrue(os.path.isdir(os.path.dirname(target)))
        # Routing applies to bare names only.
        self.assertFalse(os.path.exists(os.path.join(self.outputs, "merkle_baselines")))

    def test_defaults_reach_the_binary(self):
        # `python3 run.py` and `python3 run.py -t merkle_benchmarks` from the
        # README: the default 2^24 leaf count sits exactly on the cap and must
        # pass it, and no --benchmark-json is forwarded when none was given.
        pir = self.invoke()
        self.assertEqual(pir.events, ["build", "launch"])
        (argv, _kwargs), = pir.launches
        self.assertEqual(argv, [run.BINARY, "--test", "pir",
                                "--experiments", "5", "--warmup", "3"])

        merkle = self.invoke("-t", "merkle_benchmarks")
        self.assertEqual(merkle.events, ["build", "launch"])
        (argv, _kwargs), = merkle.launches
        self.assertEqual(argv, self.merkle_prefix() + [
            "--leaf-count", str(1 << 24), "--benchmark-case", "all"])
        self.assertFalse(os.path.exists(os.path.join(self.outputs, "merkle_baselines")))

    def test_pir_run_forwards_only_generic_flags(self):
        result = self.invoke("-t", "pir", "--no-compress", "-n", "2", "-w", "1")
        self.assertEqual(result.events, ["build", "launch"])
        (argv, _kwargs), = result.launches
        # Exact: the Merkle-only flags (--leaf-count, --benchmark-case,
        # --benchmark-json) must not leak into other tests' argv.
        self.assertEqual(argv, [run.BINARY, "--test", "pir",
                                "--experiments", "2", "--warmup", "1",
                                "--no-compress"])


if __name__ == "__main__":
    unittest.main()
