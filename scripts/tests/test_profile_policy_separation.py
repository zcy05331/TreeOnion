import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from summarize_cloudlab_profiles import collect_merkle


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


if __name__ == "__main__":
    unittest.main()
