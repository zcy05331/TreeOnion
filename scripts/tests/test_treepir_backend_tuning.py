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


class RunnerTests(unittest.TestCase):
    def test_runner_generates_index_then_tunes_candidates_outside_profiling_root(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            run_root = pathlib.Path(tmp)
            source = run_root / "source"
            (run_root / "metadata").mkdir(parents=True)
            (run_root / "metadata" / "source-commit.txt").write_text("commit\n")
            (run_root / "outputs" / "profiling" / "1gb").mkdir(parents=True)
            old_treepir = run_root / "outputs" / "profiling" / "1gb" / "treepir.json"
            old_treepir.write_text("{\"old\": true}\n")

            calls: list[tuple[list[str], pathlib.Path, pathlib.Path, dict[str, str]]] = []

            formal_leaves = [1, 2, 3, *range(10, 26)]

            def fake_run(command, *, log_path, output_path, env):
                calls.append((command, log_path, output_path, env))
                output_path.parent.mkdir(parents=True, exist_ok=True)
                log_path.parent.mkdir(parents=True, exist_ok=True)
                log_path.write_text("fake log\n")
                output_path.with_name(f"{output_path.stem}-process.json").write_text(json.dumps({"exit_code": 0, "process_peak_rss_bytes": 1234}) + "\n")
                if env.get("SUPERRECORD_PLAN_ONLY") == "1":
                    pathlib.Path(env["SUPERRECORD_OUTPUT_JSON"]).write_text(json.dumps({
                        "leaf_schedule": {
                            "warmup": [1, 2, 3],
                            "measured": list(range(10, 26)),
                        }
                    }) + "\n")
                elif "--validate-index-only" in command:
                    index_only = treepir_doc(24, [0.0] * 16, leaves=formal_leaves)
                    index_only["validate_index_only"] = True
                    for row in index_only["results"]:
                        row["path_nodes"] = [1, 2, 3]
                        row["sub_indices"] = [0, 1, 2]
                    output_path.write_text(json.dumps(index_only) + "\n")
                else:
                    label = output_path.stem
                    count = int(command[command.index("--trials") + 1])
                    leaves = formal_leaves[:3 + count]
                    mean = {
                        "pilot-vbpir_fd64": 18.0,
                        "pilot-vbpir_fd128": 15.0,
                        "pilot-vbpir_fd256": 16.0,
                        "pilot-sealpir_d1": 11.0,
                        "pilot-sealpir_d2": 14.0,
                        "formal": 9.0,
                    }[label]
                    output_path.write_text(json.dumps(treepir_doc(24, [mean] * count, leaves=leaves)) + "\n")
                return json.loads(output_path.with_name(f"{output_path.stem}-process.json").read_text())

            with contextlib.redirect_stdout(io.StringIO()):
                code = tuning.main([
                    "--run-root", str(run_root),
                    "--heights", "24",
                    "--trials", "16",
                    "--warmups", "3",
                    "--pilot-trials", "4",
                ], runner=fake_run)

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
            self.assertEqual((tuning_root / "treepir-leaves-tuned.txt").read_text().splitlines(), [str(x) for x in [1, 2, 3, *range(10, 26)]])
            command_names = [path.stem for _, _, path, _ in calls]
            self.assertEqual(command_names[0], "schedule-treepir-tuned")
            self.assertEqual(command_names[1], "vbpir-index-only")
            self.assertEqual(command_names[2:], ["pilot-vbpir_fd64", "pilot-vbpir_fd128", "pilot-vbpir_fd256", "pilot-sealpir_d1", "pilot-sealpir_d2", "formal"])
            self.assertEqual(calls[-1][0][calls[-1][0].index("--trials") + 1], "16")
            self.assertIn("--first-dim", calls[2][0])
            self.assertIn("--pir-d", calls[-1][0])


if __name__ == "__main__":
    unittest.main()
