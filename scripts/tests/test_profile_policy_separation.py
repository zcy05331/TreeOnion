import sys
import tempfile
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from summarize_cloudlab_profiles import collect_merkle, collect_treepir, directory_files, scale_from_path


class ProfilePolicySeparationTest(unittest.TestCase):
    def rows(self, policy, elapsed):
        case = {
            "name": "merkle_layerwise",
            "layer_layout_policy": policy,
            "pipeline_profile_samples_ms": [{"stages": {"first_dim": elapsed}}],
            "server_compute_samples_ms": [elapsed],
            "setup_ms": 0,
        }
        return collect_merkle({"cases": [case]}, Path("layerwise.json"), "1gb", None)

    def test_tuned_and_legacy_samples_do_not_merge(self):
        rows = self.rows("legacy", 100) + self.rows("profiled", 75)
        server = [row for row in rows if row["stage"] == "server_compute_ms"]
        self.assertEqual(len({(row["case"], row["variant"]) for row in server}), 2)
        self.assertEqual({row["value"] for row in server}, {100, 75})

    def test_legacy_name_stays_stable(self):
        self.assertEqual({row["variant"] for row in self.rows("legacy", 100)},
                         {"merkle_layerwise"})

    def test_directory_files_discovers_all_supported_scale_directories(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            for scale in ("1gb", "4gb", "8gb"):
                (root / scale).mkdir()
                (root / scale / "generic.json").write_text("{}\n")
                (root / scale / "generic-process.json").write_text("{}\n")
                (root / scale / "schedule.json").write_text("{}\n")
            self.assertEqual(
                [path.relative_to(root).as_posix() for path in directory_files(root)],
                ["1gb/generic.json", "4gb/generic.json", "8gb/generic.json"],
            )

    def test_directory_files_uses_root_json_fallback_when_no_scale_directories_exist(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            (root / "generic.json").write_text("{}\n")
            (root / "profile_summary.json").write_text("{}\n")
            (root / "generic-process.json").write_text("{}\n")
            self.assertEqual(
                [path.name for path in directory_files(root)],
                ["generic.json"],
            )

    def test_scale_from_path_labels_4gib_outputs(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = Path(tmp)
            profile = root / "4gb" / "generic.json"
            profile.parent.mkdir()
            profile.write_text("{}\n")
            self.assertEqual(scale_from_path(profile, root), "4gb")

    def test_sealpir_crypto_and_setup_stages_reach_summary(self):
        stages = {name: 1.0 for name in [
            "server_expansion_ms", "server_query_ntt_ms", "server_database_ntt_ms",
            "server_multiply_accumulate_ms", "server_inverse_ntt_ms",
            "server_recursive_decomposition_modswitch_ms", "server_modswitch_ms"]}
        rows = collect_treepir({
            "offline_key_setup_ms": 4.0, "database_setup_ms": 5.0,
            "results": [{"phase": "measured", "server_ms": 8.0, **stages}],
        }, Path("treepir.json"), "4gb", None)
        actual = {row["stage"]: row["value"] for row in rows}
        for name,value in stages.items():
            self.assertEqual(actual[name], value)
        self.assertEqual(actual["offline_key_setup_ms"], 4.0)
        self.assertEqual(actual["database_setup_ms"], 5.0)

    def test_treepir_backend_label_is_preserved_as_variant(self):
        rows = collect_treepir({
            "backend_label": "sealpir_d1",
            "results": [{
                "phase": "measured",
                "correct": True,
                "query_ms": 1.0,
                "server_ms": 2.0,
                "decode_ms": 3.0,
            }],
        }, Path("treepir.json"), "4gb", None)
        server = [row for row in rows if row["stage"] == "server_ms"]
        self.assertEqual({row["variant"] for row in server}, {"sealpir_d1"})


if __name__ == "__main__":
    unittest.main()
