import pathlib
import sys
import unittest

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from run_tree_t257_comparison import validate_result, compare_results


def result(t=4099):
    return {
        'leaf_count': 4096, 'node_entry_bits': 256,
        'actual_logical_bytes': (8192 - 1) * 32,
        'warmups': 1, 'measured_trials': 2,
        'metadata': {'plaintext_modulus': str(t), 'chunk_bits': '12' if t == 4099 else '8',
                     'response_modulus': '4169729', 'ciphertext_modulus': '123456789',
                     'ring_degree': '2048', 'g': '32',
                     'N0': '64', 'b': '0', 'key_height': '10',
                     'min_noise_budget_bits': '2', 'max_noise': '100'},
        'samples': [{'trial': i, 'leaf': i, 'correct': True,
                     'client_query_ms': 1, 'server_total_ms': 100 if t == 4099 else 80,
                     'response_serialize_ms': 1, 'client_decode_ms': 1,
                     'stages_ms': {'scan_core': 60 if t == 4099 else 40}}
                    for i in range(2)],
        'communication': {},
    }


class ComparisonTests(unittest.TestCase):
    def test_wrong_modulus_cannot_be_labeled_candidate(self):
        with self.assertRaisesRegex(ValueError, 'plaintext_modulus'):
            validate_result(result(), pathlib.Path('candidate.json'), 257, 4096, 2, 1)

    def test_failed_noise_margin_is_rejected(self):
        raw = result(257)
        raw['metadata']['min_noise_budget_bits'] = '0'
        with self.assertRaisesRegex(ValueError, 'noise'):
            validate_result(raw, pathlib.Path('candidate.json'), 257, 4096, 2, 1)

    def test_stage_and_throughput_comparison_uses_actual_bytes(self):
        comparison = compare_results(result(), result(257))
        self.assertEqual(comparison['speedup'], 1.25)
        self.assertAlmostEqual(comparison['candidate_mib_s'], (8191 * 32) / 2**20 / .08)
        self.assertEqual(comparison['stage_rows'][0]['saved_ms'], 20)

    def test_layout_change_is_not_a_modulus_only_comparison(self):
        candidate = result(257)
        candidate['metadata']['N0'] = '128'
        with self.assertRaisesRegex(ValueError, 'N0'):
            compare_results(result(), candidate)

    def test_different_query_schedule_is_rejected(self):
        candidate = result(257)
        candidate['samples'][0]['leaf'] = 7
        with self.assertRaisesRegex(ValueError, 'schedule'):
            compare_results(result(), candidate)


if __name__ == '__main__':
    unittest.main()
