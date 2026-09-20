"""Tests for summarize_cloudlab_profiles.py.

Covers the sampled-profile collector for cuckoo_bench communication shapes,
the header-derived trial count that ``--directory`` relies on when the matrix
summarizes mixed 50/8-sample files, and the
directory summary end to end (aggregation, peak-RSS join, file exclusions,
markdown whitelist).  Fixtures follow what the C++ writers actually emit
(src/tests/test_cuckoo_batch.cpp, src/merkle_benchmark.cpp and the TreePIR
adapter).
"""

import csv
import json
import pathlib
import subprocess
import sys
import tempfile
import unittest

SCRIPTS = pathlib.Path(__file__).resolve().parents[1]
SCRIPT = SCRIPTS / "summarize_cloudlab_profiles.py"
sys.path.insert(0, str(SCRIPTS))
from summarize_cloudlab_profiles import (  # noqa: E402
    ProfileError,
    collect_rows,
    collect_sampled,
    validate_profile,
    validate_sampled,
)


def cuckoo_profile(samples=3, height=10, leaf_count=4096):
    """Shape written by test_cuckoo_batch.cpp (``--test cuckoo_bench``): one
    communication block with plain names, no per-sample response bytes and no
    diagnostics_ms."""
    layout = {"target_num_pt": 86, "h": height, "expansion_height": height, "N0": 1 << height,
              "Nrest": 1, "num_pt": 96, "padded_plaintexts": 96, "padding_plaintexts": 10}
    return {
        "schema_version": "cuckoo_raw_profile.v1",
        "scheme": "cuckoo_batch_pir_1_5x_plus_one",
        "bucket_sizing": "ceil(1.5*k)+1",
        "hash_seed": 123,
        "source_commit": None,
        "leaf_count": leaf_count,
        "tree_height": 12,
        "node_entry_bits": 256,
        "actual_logical_bytes": (2 * leaf_count - 2) * 32,
        "root_excluded_node_count": 2 * leaf_count - 2,
        "warmups": 1,
        "measured_trials": samples,
        "trial_seed": 7,
        "batch_size": 12,
        "num_buckets": 19,
        "num_hashes": 3,
        "layout_policy": "forced_expansion_height",
        "requested_expansion_height": height,
        "offline_setup_ms": 40.0,
        "setup_ms": 40.0,
        "reference_layout": layout,
        "bucket_layouts": [{"bucket": 0, "member_count": 430, "layout": layout}],
        "stage_map": ["expand", "convert", "first_dim_query_ntt", "first_dim_query_pack",
                      "first_dim_core", "first_dim_finalize", "other_dim", "mod_switch"],
        "communication": {
            "modeled_query_bytes": 595200,
            "actual_response_bytes": 450560,
            "modeled_helper_key_bytes": 1488000,
        },
        "storage": {
            "bucket_count": 19, "total_member_entries": 8190, "total_target_plaintexts": 86,
            "total_padded_plaintexts": 96, "logical_padded_bytes": 4000,
            "estimated_preprocessed_bytes": 3000, "estimated_peak_resident_bytes": 5000,
        },
        "samples": [
            {
                "trial": i,
                "leaf": 100 + i,
                "bucket_routing_ms": 0.01,
                "server_total_ms": 20.0,
                "client_query_ms": 1.0,
                "response_serialize_ms": 1.0,
                "client_decode_ms": 1.0,
                "stages_ms": {
                    "expand": 3.0, "convert": 0.5, "first_dim_query_ntt": 0.3,
                    "first_dim_query_pack": 0.6, "first_dim_core": 6.0,
                    "first_dim_finalize": 0.6, "other_dim": 1.3, "mod_switch": 0.01,
                },
                "correct": True,
            }
            for i in range(samples)
        ],
    }


def merkle_profile(trials=4, leaf_count=256):
    """Shape written by merkle_benchmark.cpp benchmark_report_json for
    ``--benchmark-case merkle_paths`` (the matrix's flat.json).  The expected
    trial count lives in ``workload.measured_trials``."""
    leaves = [1 + i for i in range(trials)]
    server = [10.0 * (i + 1) for i in range(trials)]
    return {
        "schema_version": "onionpir-merkle-baselines-v2",
        "environment": {"commit": "abc", "config": "CONFIG_N2048_K1_COMP"},
        "paper_alignment": {"paper": "OnionPIR"},
        "workload": {
            "leaf_count": leaf_count, "tree_height": 8, "node_bytes": 32,
            "nodes_per_plaintext": 96, "paper_row": "custom", "warmups": 1,
            "measured_trials": trials, "trial_seed": 7, "warmup_leaf_indices": [5],
            "trial_leaf_indices": leaves, "optional_workloads": [],
        },
        "cases": [
            {
                "name": "merkle_flat",
                "correctness_passed": True,
                "pir_call_count": 1,
                "setup_ms": 3.0,
                "client_query_ms": 0.5,
                "server_compute_ms": sum(server) / trials,
                "sample_leaf_indices": leaves,
                "server_compute_samples_ms": server,
                "client_query_samples_ms": [0.5] * trials,
                "response_serialize_samples_ms": [0.5] * trials,
                "response_load_decrypt_extract_samples_ms": [0.5] * trials,
                "server_phase_ms": {},
                "pipeline_profile_ms": {"expand": 1.0, "first_dim_core": 5.0},
                "pipeline_profile_samples_ms": [
                    {"leaf": leaf, "stages": {"expand": 1.0, "first_dim_core": 5.0}}
                    for leaf in leaves
                ],
                "layer_layout_policy": "",
                "layers": [],
                "response_serialize_ms": 0.5,
                "response_load_decrypt_extract_ms": 0.5,
                "online_query_bytes_modeled": 100,
                "online_response_bytes_actual": 200,
                "online_total_bytes_mixed": 300,
                "helper_key_bytes_modeled": 1000,
                "first_session_total_bytes_mixed": 1300,
            }
        ],
    }


def treepir_profile(trials=2, measured=2):
    """TreePIR driver output: the expected trial count is the top-level ``trials``."""
    def result(i, phase):
        return {
            "iteration": i, "phase": phase, "leaf_id": 10 + i, "correct": True,
            "query_ms": 1.0, "query_serialize_ms": 0.1, "server_ms": 50.0,
            "server_expansion_ms": 10.0, "response_serialize_ms": 0.2, "decode_ms": 0.5,
            "actual_query_payload_bytes_unseeded": 2000, "actual_response_payload_bytes": 1500,
        }
    return {
        "model": "TreePIR+SealPIRplusCore",
        "backend_label": "sealpir_d2",
        "leaf_count": 4096,
        "height": 12,
        "warmups": 1,
        "trials": trials,
        "offline_key_setup_ms": 4.0,
        "database_setup_ms": 5.0,
        "actual_helper_key_bytes_uncompressed": 8000,
        "results": [result(0, "warmup")] + [result(1 + i, "measured") for i in range(measured)],
        "measured_summary": {"count": measured, "all_correct": True, "server_ms_mean": 50.0},
    }


def process_record(peak_rss_bytes):
    """read_process_peak_memory.run_measured record, as the wrapper writes it."""
    return {
        "exit_code": 0,
        "elapsed_seconds": 12.5,
        "process_peak_rss_bytes": peak_rss_bytes,
        "ru_maxrss_unit": "KiB",
        "peak_scope": "single benchmark child process; includes setup, calibration and all candidates",
        "cgroup_memory_peak_bytes": None,
        "cgroup_peak_scope": "shared cgroup cumulative peak, when available",
    }


def write_json(path, data):
    path.write_text(json.dumps(data) + "\n")
    return path


def rows_for(rows, stage):
    return [row for row in rows if row["stage"] == stage]


class CollectSampledTest(unittest.TestCase):
    def single(self, rows, stage):
        matching = rows_for(rows, stage)
        self.assertEqual(len(matching), 1, f"{stage}: {matching}")
        return matching[0]["value"], matching[0]["basis"], matching[0]["sample_index"]

    def test_cuckoo_response_bytes_come_from_communication_exactly_once(self):
        data = cuckoo_profile(samples=3)
        path = pathlib.Path("1gb/cuckoo_tuned.json")
        validate_sampled(data, path, None, None)
        self.assertEqual(validate_profile(data, path, 3, 4096), "sampled")
        rows = collect_sampled(data, path, "1gb", None)
        # The cuckoo writer records its scheme, so the family is that string and the
        # case name is the file stem the matrix chose.
        self.assertEqual({row["family"] for row in rows}, {"cuckoo_batch_pir_1_5x_plus_one"})
        self.assertEqual({row["case"] for row in rows}, {"cuckoo_tuned"})
        self.assertEqual(self.single(rows, "response_download_bytes"), (450560.0, "actual", "communication"))
        self.assertEqual(self.single(rows, "query_upload_bytes"), (595200.0, "modeled", "communication"))
        self.assertEqual(self.single(rows, "online_total_bytes_mixed"), (595200.0 + 450560.0, "mixed", "communication"))
        self.assertEqual(self.single(rows, "first_session_modeled_query_plus_helper_bytes"),
                         (595200.0 + 1488000.0, "modeled", "communication"))
        self.assertEqual(self.single(rows, "helper_key_bytes"), (1488000.0, "modeled", "communication"))
        self.assertEqual(rows_for(rows, "compression_key_bytes"), [])
        setup = rows_for(rows, "offline_setup_ms")
        self.assertEqual([(row["value"], row["kind"], row["sample_index"]) for row in setup], [(40.0, "offline", "setup")])
        self.assertEqual([row for row in rows if row["kind"] == "diagnostic"], [])
        self.assertEqual(len(rows_for(rows, "server_total_ms")), 3)
        self.assertEqual(len(rows_for(rows, "first_dim_core")), 3)


class HeaderTrialCountTest(unittest.TestCase):
    """``--directory`` runs without --trials (run_cloudlab_profile_matrix.sh): the
    expected sample count has to come from each file's own header, and a file whose
    arrays are shorter than that header must be rejected, not summarized."""

    def test_merkle_expected_count_comes_from_workload_measured_trials(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            rows = collect_rows(write_json(root / "flat.json", merkle_profile(trials=4)), root, None)
            self.assertEqual(len(rows_for(rows, "server_compute_ms")), 4)
            truncated = merkle_profile(trials=4)
            truncated["cases"][0]["server_compute_samples_ms"] = [10.0, 20.0, 30.0]
            path = write_json(root / "flat-short.json", truncated)
            # Regression: reading only the top-level measured_trials key leaves expected=None
            # for merkle files and the short array would be averaged without complaint.
            with self.assertRaisesRegex(ProfileError, r"server_compute_samples_ms count 3 != expected 4"):
                collect_rows(path, root, None)

    def test_treepir_expected_count_comes_from_top_level_trials(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            path = write_json(root / "treepir.json", treepir_profile(trials=2, measured=1))
            with self.assertRaisesRegex(ProfileError, r"measured results count 1 != expected 2"):
                collect_rows(path, root, None)
            rows = collect_rows(write_json(root / "treepir-ok.json", treepir_profile(trials=2, measured=2)), root, None)
            self.assertEqual(len(rows_for(rows, "server_ms")), 2)

    def test_sampled_expected_count_comes_from_measured_trials(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            rows = collect_rows(write_json(root / "cuckoo_tuned.json", cuckoo_profile(samples=2)), root, None)
            self.assertEqual(len(rows_for(rows, "server_total_ms")), 2)
            # Regression: sampled files are the only family whose count lives in the
            # top-level measured_trials; losing that branch leaves expected=None here and a
            # samples array shorter than the header is summarized instead of rejected.
            short = cuckoo_profile(samples=2)
            short["measured_trials"] = 3
            with self.assertRaisesRegex(ProfileError, r"samples count 2 != expected 3"):
                collect_rows(write_json(root / "cuckoo_tuned-short.json", short), root, None)


class DirectorySummaryTest(unittest.TestCase):
    def test_directory_mode_aggregates_joins_peak_rss_and_skips_non_profiles(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            scale = root / "1gb"
            scale.mkdir()
            write_json(scale / "flat.json", merkle_profile(trials=4))
            write_json(scale / "flat-process.json", process_record(7000))
            write_json(scale / "cuckoo_tuned.json", cuckoo_profile(samples=4))
            write_json(scale / "cuckoo_tuned-process.json", process_record(9000))
            write_json(scale / "schedule.json", {"schema_version": "retired-plan-only"})
            (scale / "flat.log").write_text("benchmark stdout\n")

            result = subprocess.run(
                [sys.executable, str(SCRIPT), "--directory", str(root)],
                text=True, capture_output=True, check=False,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.splitlines(), [
                f"wrote {root / 'stage_samples.csv'}",
                f"wrote {root / 'profile_summary.json'}",
                f"wrote {root / 'PROFILE_SUMMARY.md'}",
            ])

            summary = json.loads((root / "profile_summary.json").read_text())
            by_key = {
                (entry["scale"], entry["case"], entry["family"], entry["variant"],
                 entry["stage"], entry["unit"], entry["basis"]): entry
                for entry in summary["summaries"]
            }
            flat = by_key[("1gb", "merkle_flat", "merkle", "merkle_flat", "server_compute_ms", "ms", "actual")]
            self.assertEqual((flat["count"], flat["mean"], flat["median"], flat["p95"]), (4, 25.0, 25.0, 40.0))
            self.assertAlmostEqual(flat["stdev"], 12.9099, places=4)
            self.assertEqual(flat["peak_rss_bytes"], 7000)
            cuckoo = by_key[("1gb", "cuckoo_tuned", "cuckoo_batch_pir_1_5x_plus_one", "", "server_total_ms", "ms", "actual")]
            self.assertEqual((cuckoo["count"], cuckoo["peak_rss_bytes"]), (4, 9000))
            self.assertEqual(by_key[("1gb", "cuckoo_tuned", "cuckoo_batch_pir_1_5x_plus_one", "", "response_download_bytes", "bytes", "actual")]["count"], 1)
            self.assertIn(("1gb", "merkle_flat", "merkle", "merkle_flat", "expand", "ms", "actual"), by_key)

            with (root / "stage_samples.csv").open(newline="") as handle:
                reader = csv.reader(handle)
                header = next(reader)
                csv_rows = list(reader)
            self.assertEqual(header, ["scale", "case", "family", "variant", "sample_index", "stage", "kind",
                                      "value", "unit", "basis", "source_file", "peak_rss_bytes"])
            sources = {pathlib.Path(row[header.index("source_file")]).name for row in csv_rows}
            self.assertEqual(sources, {"flat.json", "cuckoo_tuned.json"})
            self.assertEqual(summary["source_rows"], len(csv_rows))

            markdown = (root / "PROFILE_SUMMARY.md").read_text()
            self.assertIn("| 1gb | merkle_flat | merkle_flat | server_compute_ms | ms | actual | 4 | 25.000 | 25.000 | 40.000 | 12.910 | 7000 |", markdown)
            self.assertIn("| 1gb | cuckoo_tuned |  | server_total_ms | ms | actual | 4 |", markdown)
            # Only the whitelisted headline stages reach the markdown; expand is in the JSON.
            self.assertNotIn("| expand |", markdown)


if __name__ == "__main__":
    unittest.main()
