from __future__ import annotations

import importlib.util
import contextlib
import io
import json
import pathlib
import tempfile
import unittest


SCRIPTS = pathlib.Path(__file__).resolve().parents[1]
MODULE = SCRIPTS / "run_treepir_backend_tuning.py"
spec = importlib.util.spec_from_file_location("treepir_backend_tuning", MODULE)
tuning = importlib.util.module_from_spec(spec)
spec.loader.exec_module(tuning)


def treepir_doc(height: int, server_values: list[float], *, correct: bool = True, leaves: list[int] | None = None) -> dict:
    leaves = leaves or [7, 8, 9, *range(100, 100 + len(server_values))]
    results = [
        {"phase": "warmup", "leaf_id": leaves[0], "correct": True, "index_ms": 0.0, "server_ms": 0.0},
        {"phase": "warmup", "leaf_id": leaves[1], "correct": True, "index_ms": 0.0, "server_ms": 0.0},
        {"phase": "warmup", "leaf_id": leaves[2], "correct": True, "index_ms": 0.0, "server_ms": 0.0},
    ]
    for i, server_ms in enumerate(server_values):
        results.append({
            "phase": "measured",
            "leaf_id": leaves[3 + i],
            "correct": correct,
            "index_ms": 0.0,
            "query_ms": 0.0,
            "server_ms": server_ms,
            "decode_ms": 0.0,
            "server_first_dimension_ms": 0.0,
        })
    return {
        "model": "SEAL+VBPIRTreePIR",
        "height": height,
        "leaf_count": 1 << height,
        "warmups": 3,
        "trials": len(server_values),
        "measured_summary": {"count": len(server_values), "all_correct": correct},
        "results": results,
    }


class ValidationTests(unittest.TestCase):
    def test_validate_candidate_accepts_full_correct_positive_stage_output(self) -> None:
        candidate = tuning.Candidate("vbpir_fd64", "vbpir", {"first_dim": 64})
        leaves = [1, 2, 3, 10, 11]
        result = tuning.validate_candidate_result(candidate, treepir_doc(24, [10.0, 12.0], leaves=leaves), height=24, trials=2, warmups=3, leaves=leaves)

        self.assertTrue(result.eligible)
        self.assertEqual(result.mean_server_ms, 11.0)

    def test_validate_candidate_rejects_wrong_measured_count(self) -> None:
        candidate = tuning.Candidate("vbpir_fd64", "vbpir", {"first_dim": 64})
        leaves = [1, 2, 3, 10, 11]
        result = tuning.validate_candidate_result(candidate, treepir_doc(24, [10.0], leaves=leaves[:-1]), height=24, trials=2, warmups=3, leaves=leaves)

        self.assertFalse(result.eligible)
        self.assertIn("measured result count", result.reason)

    def test_validate_candidate_rejects_incorrect_result(self) -> None:
        candidate = tuning.Candidate("sealpir_d1", "sealpir", {"pir_d": 1})
        leaves = [1, 2, 3, 10, 11]
        result = tuning.validate_candidate_result(candidate, treepir_doc(24, [10.0, 11.0], correct=False, leaves=leaves), height=24, trials=2, warmups=3, leaves=leaves)

        self.assertFalse(result.eligible)
        self.assertIn("correctness", result.reason)

    def test_selects_lowest_finite_mean_server_time(self) -> None:
        results = [
            tuning.CandidateResult("slow", True, 20.0, "ok"),
            tuning.CandidateResult("bad", False, None, "process failed"),
            tuning.CandidateResult("fast", True, 9.5, "ok"),
        ]

        self.assertEqual(tuning.select_best_candidate(results).label, "fast")

    def test_validate_candidate_rejects_leaf_schedule_mismatch(self) -> None:
        candidate = tuning.Candidate("vbpir_fd64", "vbpir", {"first_dim": 64})
        result = tuning.validate_candidate_result(candidate, treepir_doc(24, [10.0, 12.0], leaves=[1, 2, 3, 10, 12]), height=24, trials=2, warmups=3, leaves=[1, 2, 3, 10, 11])

        self.assertFalse(result.eligible)
        self.assertIn("leaf schedule", result.reason)

    def test_height_27_drops_the_first_dim_64_vbpir_variant(self) -> None:
        # The 8 GiB point tunes only the VBPIR dims that fit; the SealPIR pair is unchanged.
        self.assertEqual([candidate.label for candidate in tuning.candidates_for_height(27)],
                         ["vbpir_fd128", "vbpir_fd256", "sealpir_d1", "sealpir_d2"])
        self.assertEqual([candidate.label for candidate in tuning.candidates_for_height(24)],
                         ["vbpir_fd64", "vbpir_fd128", "vbpir_fd256", "sealpir_d1", "sealpir_d2"])


FORMAL_LEAVES = tuning.trial_leaves(1 << 24, 3, 16, 7)
PILOT_MEAN_MS = {
    "pilot-vbpir_fd64": 18.0,
    "pilot-vbpir_fd128": 15.0,
    "pilot-vbpir_fd256": 16.0,
    "pilot-sealpir_d1": 11.0,
    "pilot-sealpir_d2": 14.0,
    "formal": 9.0,
}
TUNING_ARGS = ["--heights", "24", "--trials", "16", "--warmups", "3", "--pilot-trials", "4"]


def make_run_root(tmp: str) -> pathlib.Path:
    run_root = pathlib.Path(tmp)
    (run_root / "metadata").mkdir(parents=True)
    (run_root / "metadata" / "source-commit.txt").write_text("commit\n")
    (run_root / "outputs" / "profiling" / "1gb").mkdir(parents=True)
    return run_root


def make_fake_run(calls: list, *, failing_pilots: frozenset[str] = frozenset()):
    """Runner double for height 24: answers the index-only, pilot and formal launches.

    A pilot whose output stem is in failing_pilots exits 1 and writes no output, like a
    backend variant that crashed or was killed."""

    def fake_run(command, *, log_path, output_path, env):
        calls.append((command, log_path, output_path, env))
        output_path.parent.mkdir(parents=True, exist_ok=True)
        log_path.parent.mkdir(parents=True, exist_ok=True)
        log_path.write_text("fake log\n")
        process_path = output_path.with_name(f"{output_path.stem}-process.json")
        if output_path.stem in failing_pilots:
            process_path.write_text(json.dumps({"exit_code": 1, "process_peak_rss_bytes": 1234}) + "\n")
            return json.loads(process_path.read_text())
        process_path.write_text(json.dumps({"exit_code": 0, "process_peak_rss_bytes": 1234}) + "\n")
        if "--validate-index-only" in command:
            index_only = treepir_doc(24, [0.0] * 16, leaves=FORMAL_LEAVES)
            index_only["validate_index_only"] = True
            for row in index_only["results"]:
                row["path_nodes"] = [1, 2, 3]
                row["sub_indices"] = [0, 1, 2]
            output_path.write_text(json.dumps(index_only) + "\n")
        else:
            label = output_path.stem
            count = int(command[command.index("--trials") + 1])
            leaves = FORMAL_LEAVES[:3 + count]
            output_path.write_text(json.dumps(treepir_doc(24, [PILOT_MEAN_MS[label]] * count, leaves=leaves)) + "\n")
        return json.loads(process_path.read_text())

    return fake_run


class RunnerTests(unittest.TestCase):
    def test_runner_generates_index_then_tunes_candidates_outside_profiling_root(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            run_root = make_run_root(tmp)
            old_treepir = run_root / "outputs" / "profiling" / "1gb" / "treepir.json"
            old_treepir.write_text("{\"old\": true}\n")

            calls: list[tuple[list[str], pathlib.Path, pathlib.Path, dict[str, str]]] = []

            with contextlib.redirect_stdout(io.StringIO()):
                code = tuning.main(["--run-root", str(run_root), *TUNING_ARGS], runner=make_fake_run(calls))

            self.assertEqual(code, 0)
            self.assertEqual(old_treepir.read_text(), "{\"old\": true}\n")
            tuning_root = run_root / "outputs" / "treepir-backend-tuning" / "1gb"
            final = json.loads((tuning_root / "formal.json").read_text())
            self.assertEqual(final["backend_label"], "sealpir_d1")
            self.assertEqual(final["comparison_regime"]["global_optimality_claim"], "none")
            self.assertEqual(final["comparison_regime"]["formal_trials"], 16)
            self.assertEqual(final["comparison_regime"]["pilot_trials"], 4)
            self.assertEqual(len(final["candidate_eligibility"]), 5)
            self.assertTrue((tuning_root / "formal-process.json").exists())
            self.assertTrue((tuning_root / "formal.log").exists())
            self.assertFalse((run_root / "outputs" / "profiling" / "1gb" / "treepir-tuned.json").exists())
            self.assertEqual((tuning_root / "treepir-leaves-tuned.txt").read_text().splitlines(), [str(x) for x in FORMAL_LEAVES])
            command_names = [path.stem for _, _, path, _ in calls]
            self.assertEqual(command_names[0], "vbpir-index-only")
            self.assertEqual(command_names[1:], ["pilot-vbpir_fd64", "pilot-vbpir_fd128", "pilot-vbpir_fd256", "pilot-sealpir_d1", "pilot-sealpir_d2", "formal"])
            self.assertEqual(calls[-1][0][calls[-1][0].index("--trials") + 1], "16")
            self.assertIn("--first-dim", calls[1][0])
            self.assertIn("--pir-d", calls[-1][0])

    def test_crashed_pilot_is_recorded_ineligible_and_tuning_continues(self) -> None:
        # One backend variant dying must not end a multi-hour tuning session: the pilot is
        # recorded as "process exit N" and the formal run selects among the survivors.
        with tempfile.TemporaryDirectory() as tmp:
            run_root = make_run_root(tmp)
            calls: list = []

            with contextlib.redirect_stdout(io.StringIO()):
                code = tuning.main(["--run-root", str(run_root), *TUNING_ARGS],
                                   runner=make_fake_run(calls, failing_pilots=frozenset({"pilot-vbpir_fd64"})))

            self.assertEqual(code, 0)
            tuning_root = run_root / "outputs" / "treepir-backend-tuning" / "1gb"
            self.assertFalse((tuning_root / "candidates" / "pilot-vbpir_fd64.json").exists())
            selection = json.loads((tuning_root / "candidates" / "selection.json").read_text())
            eligibility = {entry["label"]: entry for entry in selection["candidate_eligibility"]}
            self.assertEqual(len(eligibility), 5)
            self.assertEqual(eligibility["vbpir_fd64"],
                             {"label": "vbpir_fd64", "eligible": False, "mean_server_ms": None, "reason": "process exit 1"})
            self.assertTrue(all(entry["eligible"] for label, entry in eligibility.items() if label != "vbpir_fd64"))
            self.assertEqual(selection["selected"]["label"], "sealpir_d1")
            final = json.loads((tuning_root / "formal.json").read_text())
            self.assertEqual(final["backend_label"], "sealpir_d1")
            self.assertEqual([path.stem for _, _, path, _ in calls][1:],
                             ["pilot-vbpir_fd64", "pilot-vbpir_fd128", "pilot-vbpir_fd256", "pilot-sealpir_d1", "pilot-sealpir_d2", "formal"])

    def test_all_pilots_failing_exits_nonzero_without_a_formal_run(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            run_root = make_run_root(tmp)
            calls: list = []
            stderr = io.StringIO()

            with contextlib.redirect_stdout(io.StringIO()), contextlib.redirect_stderr(stderr):
                code = tuning.main(["--run-root", str(run_root), *TUNING_ARGS],
                                   runner=make_fake_run(calls, failing_pilots=frozenset(PILOT_MEAN_MS) - {"formal"}))

            self.assertEqual(code, 1)
            self.assertIn("no eligible TreePIR backend candidates", stderr.getvalue())
            tuning_root = run_root / "outputs" / "treepir-backend-tuning" / "1gb"
            self.assertFalse((tuning_root / "formal.json").exists())
            self.assertNotIn("formal", [path.stem for _, _, path, _ in calls])
            self.assertEqual(len(calls), 1 + 5)

    def test_unsupported_height_is_rejected_before_any_process_launch(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            run_root = make_run_root(tmp)
            calls: list = []
            stderr = io.StringIO()

            with contextlib.redirect_stderr(stderr):
                code = tuning.main(["--run-root", str(run_root), "--heights", "21"], runner=make_fake_run(calls))

            self.assertEqual(code, 1)
            # By output stem, not by the raw call tuples: a regression here would otherwise
            # print a multi-kilobyte diff of commands and environments instead of the names.
            self.assertEqual([path.stem for _, _, path, _ in calls], [])
            self.assertIn("unsupported TreePIR height: 21", stderr.getvalue())
            self.assertFalse((run_root / "outputs" / "treepir-backend-tuning").exists())


if __name__ == "__main__":
    unittest.main()
