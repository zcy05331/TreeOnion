import copy
import json
import pathlib
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import benchmark_gadget_configs as gadget


def profile(*, n0=352, label="bundle3", samples=2, packing="projection", include_merge=False):
    sample = {
        "block_id": 0,
        "leaf": 11,
        "server_total_ms": 10.0,
        "client_query_ms": 1.0,
        "client_decode_ms": 1.0,
        "response_serialize_ms": 1.0,
        "stages_ms": {
            "expand": 0.5,
            "complete": 0.25,
            "prepare": 1.0,
            "scan": 2.0,
            "finalize": 0.5,
            "fold": 1.0,
            "rotate": 1.25,
            "project": 0.75,
            "pack": 0.5,
            "switch": 0.25,
        },
        "response_merge_keyswitches": 0,
    }
    if include_merge:
        sample["stages_ms"]["merge"] = 1.5
        sample["response_merge_keyswitches"] = 4
    rows = [copy.deepcopy(sample) for _ in range(samples)]
    for block_id, row in enumerate(rows):
        row["block_id"] = block_id
        row["leaf"] = 11 + block_id
    return {
        "leaf_count": 1 << 22,
        "entry_bits": 256,
        "measured_trials": samples,
        "warmup_count": 3,
        "response_packing": packing,
        "candidates": [
            {
                "label": label,
                "eligible": True,
                "shape": {
                    "entry_bits": 256,
                    "leaf_count": 1 << 22,
                    "ring_degree": 2048,
                    "bundle_arity": 3,
                    "record_coeffs": 64,
                    "plane_count": 1,
                    "records_per_plaintext": 42,
                    "N0": n0,
                    "beta_bits": 7,
                    "query_width": n0 + 69,
                    "expansion_height": 9,
                },
                "candidate_metrics": {
                    "rotate_cmux_count": 3,
                    "projection_keyswitch_count": 2,
                    "modeled_per_client_eval_key_bytes": 1234,
                    "eval_key_coefficient_payload_bytes_estimate": 5678,
                },
                "scheme_fingerprint": {
                    "identity": {
                        "ring_degree": 2048,
                        "plaintext_modulus": 257,
                        "rns_modulus_bits": [50, 49],
                        "rns_moduli": [1125899906826241, 562949953421312],
                        "response_modulus": 40961,
                        "selector_gadget_rows": 3,
                    },
                    "registered_session_key_height": 11,
                },
                "level_plans": [{"level": 0, "projection_depth": 2}],
                "wire": {
                    "modeled_seeded_query_bytes": 100,
                    "actual_response_ciphertext_payload_bytes": 200,
                },
                "target_noise_result": {
                    "decrypts_correctly": True,
                    "observed_key_coverage_matches_tag": True,
                    "response_packing": packing,
                },
                "samples": rows,
            }
        ],
    }


class GadgetConfigTests(unittest.TestCase):
    def test_fixed_layout_mismatch_is_rejected(self):
        first = gadget.validate_profile(profile(n0=352), pathlib.Path("a.json"), 2, 1 << 22, (3, 64, 1, 352, 7), 9)
        second = gadget.validate_profile(profile(n0=176), pathlib.Path("b.json"), 2, 1 << 22, (3, 64, 1, 176, 7), 9)
        with self.assertRaisesRegex(ValueError, "fixed layout"):
            gadget.check_comparable(first, second, "changed")

    def test_failed_point_is_preserved_but_excluded_from_summary(self):
        def run_measured(command, *, log_path, env=None, cwd=None):
            raw = pathlib.Path(env["SUPERRECORD_OUTPUT_JSON"])
            if "bad.json" in str(raw):
                pathlib.Path(log_path).write_text("failed\n")
                return {"exit_code": 2, "elapsed_seconds": 0.1, "process_peak_rss_bytes": 10}
            raw.write_text(json.dumps(profile(label="bundle3")) + "\n")
            pathlib.Path(log_path).write_text("ok\n")
            return {"exit_code": 0, "elapsed_seconds": 0.2, "process_peak_rss_bytes": 20}

        with tempfile.TemporaryDirectory() as tmp:
            argv = [
                "benchmark",
                "--binary",
                "bad=/tmp/bad",
                "--binary",
                "good=/tmp/good",
                "--output",
                tmp,
                "--experiments",
                "2",
                "--rounds",
                "1",
            ]
            with patch.object(sys, "argv", argv), patch.object(gadget, "run_measured", run_measured):
                gadget.main()

            manifest = json.loads((pathlib.Path(tmp) / "manifest.json").read_text())
            self.assertEqual([run["state"] for run in manifest["runs"]], ["failed", "complete"])
            self.assertTrue((pathlib.Path(tmp) / "round0-bad-process.json").exists())
            self.assertTrue((pathlib.Path(tmp) / "round0-bad.log").exists())
            comparison = json.loads((pathlib.Path(tmp) / "comparison.json").read_text())
            self.assertEqual(comparison["completed_labels"], ["good"])
            self.assertEqual(comparison["summary"]["good"]["completed_samples"], 2)
            self.assertNotIn("bad", comparison["summary"])

    def test_any_failed_round_excludes_the_entire_label(self):
        calls = {}

        def run_measured(command, *, log_path, env=None, cwd=None):
            raw = pathlib.Path(env["SUPERRECORD_OUTPUT_JSON"])
            label = raw.stem.split("-", 1)[1]
            calls.setdefault(label, 0)
            calls[label] += 1
            if label == "flaky" and calls[label] == 2:
                pathlib.Path(log_path).write_text("failed\n")
                return {"exit_code": 2, "elapsed_seconds": 0.1, "process_peak_rss_bytes": 10}
            packing = env.get("SUPERRECORD_RESPONSE_PACKING", "projection")
            raw.write_text(json.dumps(profile(label=label, packing=packing, include_merge=packing == "mlwe")) + "\n")
            pathlib.Path(log_path).write_text("ok\n")
            return {"exit_code": 0, "elapsed_seconds": 0.2, "process_peak_rss_bytes": 20}

        with tempfile.TemporaryDirectory() as tmp:
            argv = [
                "benchmark",
                "--binary",
                "flaky=/tmp/flaky",
                "--binary",
                "good=/tmp/good",
                "--output",
                tmp,
                "--experiments",
                "2",
                "--rounds",
                "2",
                "--source",
                "good=abc123",
                "--packing",
                "good=mlwe",
            ]
            captured_envs = []

            def capture(command, *, log_path, env=None, cwd=None):
                captured_envs.append(env.copy())
                return run_measured(command, log_path=log_path, env=env, cwd=cwd)

            with patch.object(sys, "argv", argv), patch.object(gadget, "run_measured", capture):
                gadget.main()

            manifest = json.loads((pathlib.Path(tmp) / "manifest.json").read_text())
            self.assertEqual(manifest["state"], "complete_with_failures")
            comparison = json.loads((pathlib.Path(tmp) / "comparison.json").read_text())
            self.assertEqual(comparison["state"], "complete_with_failures")
            self.assertEqual(comparison["completed_labels"], ["good"])
            self.assertNotIn("flaky", comparison["summary"])
            self.assertEqual(comparison["rejected_labels"]["flaky"], "one or more rounds failed")
            self.assertTrue(any(env.get("SUPERRECORD_COMMIT") == "abc123" for env in captured_envs))
            self.assertTrue(any(env.get("SUPERRECORD_RESPONSE_PACKING") == "mlwe" for env in captured_envs))
            self.assertFalse(any("SUPERRECORD_SOURCE" in env for env in captured_envs))

    def test_missing_rotation_or_projection_stage_is_rejected(self):
        data = profile()
        del data["candidates"][0]["samples"][0]["stages_ms"]["project"]
        with self.assertRaisesRegex(ValueError, "missing stages_ms.project"):
            gadget.validate_profile(data, pathlib.Path("profile.json"), 2, 1 << 22, (3, 64, 1, 352, 7), 9)

    def test_summary_rejects_missing_stage_instead_of_averaging_zero(self):
        candidate = profile()["candidates"][0]
        del candidate["samples"][1]["stages_ms"]["pack"]
        with self.assertRaisesRegex(ValueError, "missing stages_ms.pack"):
            gadget.summarize_candidate(candidate, [])

    def test_missing_required_identity_field_is_rejected(self):
        data = profile()
        del data["candidates"][0]["scheme_fingerprint"]["identity"]["rns_moduli"]
        candidate = gadget.validate_profile(data, pathlib.Path("profile.json"), 2, 1 << 22, (3, 64, 1, 352, 7), 9)
        with self.assertRaisesRegex(ValueError, "missing scheme identity"):
            gadget.identity_subset(candidate)

    def test_requested_mlwe_rejects_projection_raw(self):
        data = profile(packing="projection")
        with self.assertRaisesRegex(ValueError, "response packing"):
            gadget.validate_profile(
                data,
                pathlib.Path("profile.json"),
                2,
                1 << 22,
                (3, 64, 1, 352, 7),
                9,
                "mlwe",
            )

    def test_missing_merge_time_in_mlwe_is_rejected(self):
        data = profile(packing="mlwe", include_merge=False)
        with self.assertRaisesRegex(ValueError, "missing stages_ms.merge"):
            gadget.validate_profile(
                data,
                pathlib.Path("profile.json"),
                2,
                1 << 22,
                (3, 64, 1, 352, 7),
                9,
                "mlwe",
            )

    def test_response_processing_uses_present_merge_time(self):
        candidate = gadget.validate_profile(
            profile(packing="projection", include_merge=True),
            pathlib.Path("profile.json"),
            2,
            1 << 22,
            (3, 64, 1, 352, 7),
            9,
            "projection",
        )
        summary = gadget.summarize_candidate(candidate, [])
        self.assertEqual(summary["rotation_projection_ms_mean"], 2.0)
        self.assertEqual(summary["response_processing_ms_mean"], 4.0)
        self.assertEqual(summary["counter_metadata"]["response_merge_keyswitches_mean"], 4)
        self.assertEqual(summary["packing"], "projection")

    def test_response_processing_uses_zero_merge_only_for_old_projection_schema(self):
        candidate = gadget.validate_profile(
            profile(packing="projection", include_merge=False),
            pathlib.Path("profile.json"),
            2,
            1 << 22,
            (3, 64, 1, 352, 7),
            9,
            "projection",
        )
        summary = gadget.summarize_candidate(candidate, [])
        self.assertEqual(summary["response_processing_ms_mean"], 2.5)


if __name__ == "__main__":
    unittest.main()
