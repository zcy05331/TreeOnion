"""Tests for read_process_peak_memory.py, the wrapper every formal benchmark
launch goes through (run_cloudlab_profile_matrix.sh run_case, the NUMA A/B and
interlude scripts, and the run_measured import in the Python drivers).  Runs
the real wrapper around small python3 children."""

import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

SCRIPT = pathlib.Path(__file__).resolve().parents[1] / "read_process_peak_memory.py"


def run_wrapper(root, *child):
    log = root / "bench.log"
    output = root / "bench-process.json"
    argv = [sys.executable, str(SCRIPT), "--log", str(log), "--output", str(output)]
    if child:
        argv += ["--", *child]
    result = subprocess.run(argv, text=True, capture_output=True, check=False)
    return result, log, output


class ReadProcessPeakMemoryTest(unittest.TestCase):
    def test_child_exit_code_peak_rss_and_merged_streams_reach_the_record(self):
        with tempfile.TemporaryDirectory() as tmp:
            result, log, output = run_wrapper(
                pathlib.Path(tmp), sys.executable, "-c",
                "import sys; print('out'); print('err', file=sys.stderr); sys.exit(3)")
            # run_case branches on `$?`: the child's status must pass through unchanged.
            self.assertEqual(result.returncode, 3, result.stderr)
            record = json.loads(output.read_text())
            self.assertEqual(record["exit_code"], 3)
            self.assertIsInstance(record["process_peak_rss_bytes"], int)
            self.assertGreater(record["process_peak_rss_bytes"], 0)
            self.assertGreater(record["elapsed_seconds"], 0)
            self.assertIn(record["ru_maxrss_unit"], {"bytes", "KiB"})
            text = log.read_text()
            self.assertIn("out", text)
            self.assertIn("err", text)

    def test_signal_death_is_reported_as_failure_on_both_channels(self):
        # An OOM- or SIGTERM-killed benchmark must fail run_case's `code != 0` branch and
        # the analysis scripts' `exit_code == 0` check, never look like a clean run.
        with tempfile.TemporaryDirectory() as tmp:
            result, _, output = run_wrapper(
                pathlib.Path(tmp), sys.executable, "-c",
                "import os, signal; os.kill(os.getpid(), signal.SIGTERM)")
            self.assertEqual(result.returncode, 143, result.stderr)
            self.assertEqual(json.loads(output.read_text())["exit_code"], -15)

    def test_missing_command_is_rejected_before_anything_runs(self):
        with tempfile.TemporaryDirectory() as tmp:
            result, log, output = run_wrapper(pathlib.Path(tmp))
            self.assertEqual(result.returncode, 2)
            self.assertIn("benchmark command is required", result.stderr)
            self.assertFalse(output.exists())
            self.assertFalse(log.exists())


if __name__ == "__main__":
    unittest.main()
