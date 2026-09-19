import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from path_baselines.workload import node_bytes, path_indices, record_bytes, trial_leaves


class PathWorkloadTests(unittest.TestCase):
    def test_full_ancestor_path_includes_root(self):
        self.assertEqual(path_indices(3, 6), [0, 2, 6, 13])
        self.assertEqual(path_indices(3, 0), [0, 1, 3, 7])
        self.assertEqual(path_indices(3, 7), [0, 2, 6, 14])

    def test_flat_records_match_level_coordinates(self):
        for level in range(9):
            for index in range(1 << level):
                self.assertEqual(record_bytes((1 << level) - 1 + index), node_bytes(level, index))
        self.assertEqual(len(node_bytes(24, 16777215)), 32)

    def test_existing_cpp_floyd_schedule(self):
        # Existing Generic 1GiB EP2, seed7, 3 warmups and 50 measured samples.
        leaves = trial_leaves(1 << 24, 3, 50, 7)
        self.assertEqual(leaves[3], 12995598)
        self.assertEqual(len(leaves), 53)
        self.assertEqual(len(set(leaves)), 53)
        self.assertTrue(all(0 <= x < 1 << 24 for x in leaves))

    def test_invalid_workload_is_rejected(self):
        for h, leaf in [(0, 0), (3, 8), (3, -1)]:
            with self.assertRaises(ValueError):
                path_indices(h, leaf)
        with self.assertRaises(ValueError):
            trial_leaves(8, 3, 6, 7)


if __name__ == '__main__':
    unittest.main()
