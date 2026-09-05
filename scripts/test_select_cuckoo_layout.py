#!/usr/bin/env python3
"""Unit tests for select_cuckoo_layout.py."""

from __future__ import annotations

import json
import subprocess
import tempfile
import unittest
from pathlib import Path


SCRIPT = Path(__file__).with_name("select_cuckoo_layout.py")


def write_pilot(root: Path, height: int, samples: list[float], leaf_offset: int = 0) -> Path:
    path = root / f"h{height}.json"
    leaves = [leaf_offset + i for i in range(len(samples))]
    path.write_text(
        json.dumps(
            {
                "schema_version": "cuckoo_raw_profile.v1",
                "layout_policy": "forced_expansion_height",
                "requested_expansion_height": height,
                "leaf_count": 1 << 24,
                "tree_height": 24,
                "hash_seed": 123,
                "trial_seed": 7,
                "warmups": 3,
                "measured_trials": len(samples),
                "bucket_sizing": "ceil(1.5*k)+1",
                "batch_size": 24,
                "num_buckets": 37,
                "num_hashes": 3,
                "reference_layout": {"target_num_pt": 349526, "h": 10},
                "bucket_layouts": [
                    {"bucket": 0, "member_count": 100, "layout": {"h": height}},
                    {"bucket": 1, "member_count": 120, "layout": {"h": height}},
                ],
                "communication": {
                    "modeled_query_bytes": 1000 + height,
                    "actual_response_bytes": 2000 + height,
                },
                "storage": {
                    "logical_padded_bytes": 4000 + height,
                    "estimated_preprocessed_bytes": 3000 + height,
                },
                "samples": [
                    {
                        "trial": i,
                        "leaf": leaves[i],
                        "server_total_ms": value,
                        "client_query_ms": 1.0,
                        "response_serialize_ms": 1.0,
                        "client_decode_ms": 1.0,
                        "stages_ms": {"expand": 1.0},
                        "correct": True,
                    }
                    for i, value in enumerate(samples)
                ],
            }
        )
        + "\n"
    )
    path.with_name(f"h{height}-process.json").write_text(
        json.dumps({"exit_code": 0}) + "\n"
    )
    return path


class SelectCuckooLayoutTest(unittest.TestCase):
    def test_selects_lowest_mean_server_time(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            paths = [
                write_pilot(root, 7, [10.0, 11.0, 12.0, 13.0]),
                write_pilot(root, 8, [7.0, 8.0, 9.0, 10.0]),
                write_pilot(root, 9, [8.0, 9.0, 10.0, 11.0]),
                write_pilot(root, 10, [9.0, 10.0, 11.0, 12.0]),
            ]
            output = root / "selection.json"
            result = subprocess.run(
                [
                    "python3",
                    str(SCRIPT),
                    "--output",
                    str(output),
                    "--expected-trials",
                    "4",
                    "--expected-warmups",
                    "3",
                    "--leaf-count",
                    str(1 << 24),
                    *map(str, paths),
                ],
                check=False,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.strip(), "8")
            selection = json.loads(output.read_text())
            self.assertEqual(selection["decision"]["chosen_expansion_height"], 8)
            self.assertEqual(selection["decision"]["score"], "min_mean_complete_path_server_ms")

    def test_mean_wins_when_mean_and_median_disagree(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            paths = [
                write_pilot(root, 7, [1.0, 1.0, 1.0, 200.0]),
                write_pilot(root, 8, [40.0, 40.0, 40.0, 40.0]),
                write_pilot(root, 9, [60.0, 60.0, 60.0, 60.0]),
                write_pilot(root, 10, [70.0, 70.0, 70.0, 70.0]),
            ]
            output = root / "selection.json"
            result = subprocess.run(
                [
                    "python3",
                    str(SCRIPT),
                    "--output",
                    str(output),
                    "--expected-trials",
                    "4",
                    *map(str, paths),
                ],
                check=False,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            self.assertEqual(result.returncode, 0, result.stderr)
            self.assertEqual(result.stdout.strip(), "8")
            selection = json.loads(output.read_text())
            self.assertEqual(selection["decision"]["chosen_expansion_height"], 8)
            pilot_by_height = {
                pilot["requested_expansion_height"]: pilot
                for pilot in selection["pilots"]
            }
            self.assertLess(pilot_by_height[7]["median_server_ms"], pilot_by_height[8]["median_server_ms"])
            self.assertGreater(pilot_by_height[7]["mean_server_ms"], pilot_by_height[8]["mean_server_ms"])

    def test_rejects_mismatched_leaf_schedule(self) -> None:
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            paths = [
                write_pilot(root, 7, [10.0, 11.0, 12.0, 13.0]),
                write_pilot(root, 8, [7.0, 8.0, 9.0, 10.0], leaf_offset=1),
                write_pilot(root, 9, [8.0, 9.0, 10.0, 11.0]),
                write_pilot(root, 10, [9.0, 10.0, 11.0, 12.0]),
            ]
            result = subprocess.run(
                [
                    "python3",
                    str(SCRIPT),
                    "--output",
                    str(root / "selection.json"),
                    "--expected-trials",
                    "4",
                    *map(str, paths),
                ],
                check=False,
                text=True,
                stdout=subprocess.PIPE,
                stderr=subprocess.PIPE,
            )
            self.assertNotEqual(result.returncode, 0)
            self.assertIn("measured leaf schedule does not match", result.stderr)


if __name__ == "__main__":
    unittest.main()
