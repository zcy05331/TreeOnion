import importlib.util
import pathlib
import unittest

MODULE = pathlib.Path(__file__).resolve().parents[1] / 'analyze_superrecord_sweep.py'
spec = importlib.util.spec_from_file_location('superrecord_analysis', MODULE)
analysis = importlib.util.module_from_spec(spec)
spec.loader.exec_module(analysis)


class StatisticsTests(unittest.TestCase):
    def test_statistics_use_arithmetic_mean_and_sample_deviation(self):
        stats = analysis.summarize([1.0, 2.0, 3.0, 4.0])
        self.assertEqual(stats['mean'], 2.5)
        self.assertEqual(stats['median'], 2.5)
        self.assertEqual(stats['p95'], 4.0)
        self.assertAlmostEqual(stats['stddev'], (5 / 3) ** 0.5)

    def test_paired_bootstrap_preserves_exact_correlated_ratio(self):
        control = [float(i + 10) for i in range(16)]
        candidate = [x / 2 for x in control]
        result = analysis.paired_bootstrap(candidate, control, seed=17, resamples=10000)
        self.assertEqual(result['candidate_over_control'], 0.5)
        self.assertEqual(result['ratio_ci95'], [0.5, 0.5])
        self.assertEqual(result['server_time_reduction_percent'], 50.0)
        self.assertEqual(result['speedup'], 2.0)
        self.assertFalse(result['statistically_unresolved'])

    def test_identical_samples_cannot_establish_improvement(self):
        values = [1., 2., 4., 8.] * 4
        result = analysis.paired_bootstrap(values, values, seed=1, resamples=10000)
        self.assertTrue(result['statistically_unresolved'])
        self.assertEqual(result['ratio_ci95'], [1.0, 1.0])

    def test_invalid_comparisons_are_rejected(self):
        for candidate, control in [([1.], [1., 2.]), ([0.], [1.]), ([1.], [float('nan')])]:
            with self.assertRaises(ValueError):
                analysis.paired_bootstrap(candidate, control, seed=1, resamples=10000)

    def test_small_timing_difference_uses_storage_tie_break(self):
        rows = [
            {'label': 'fast', 'server_ms': [100.] * 16, 'steady_state_bytes': 200, 'response_count': 1},
            {'label': 'small', 'server_ms': [101.] * 16, 'steady_state_bytes': 100, 'response_count': 2},
        ]
        result = analysis.select_winner(rows, seed=3, resamples=10000)
        self.assertEqual(result['selected'], 'small')
        self.assertIn('storage', result['reason'])
        self.assertEqual(result['fastest_mean'], 'fast')

    def test_response_count_breaks_storage_tie(self):
        rows = [
            {'label': 'fast', 'server_ms': [100.] * 16, 'steady_state_bytes': 100, 'response_count': 2},
            {'label': 'few', 'server_ms': [101.] * 16, 'steady_state_bytes': 101, 'response_count': 1},
        ]
        result = analysis.select_winner(rows, seed=3, resamples=10000)
        self.assertEqual(result['selected'], 'few')
        self.assertIn('response', result['reason'])

    def test_clearly_slower_small_plan_does_not_win_storage_tie(self):
        rows = [
            {'label': 'fast', 'server_ms': [100.] * 16, 'steady_state_bytes': 200, 'response_count': 2},
            {'label': 'slow', 'server_ms': [120.] * 16, 'steady_state_bytes': 100, 'response_count': 1},
        ]
        result = analysis.select_winner(rows, seed=3, resamples=10000)
        self.assertEqual(result['selected'], 'fast')


class ArtifactTests(unittest.TestCase):
    @staticmethod
    def fixture():
        rows = []
        for label, factor, h in [('legacy-g32', 2., 0), ('generic-h1', 1.5, 1), ('bundle3', 1., 3)]:
            rows.append({
                'label': label, 'eligible': True,
                'shape': {'bundle_arity': h, 'record_coeffs': 64, 'plane_count': 1, 'response_ciphertexts': 1},
                'metrics': {'optimized_steady_state_bytes_estimate': 1000 * factor},
                'samples': [{'block_id': i, 'leaf': i * 3, 'server_total_ms': (10 + i) * factor,
                             'stages_ms': {'scan': (5 + i) * factor, 'rotate': 0.1}} for i in range(16)]})
        return {'warmup_count': 3, 'candidates': rows, 'measurement_schedule': 'interleaved_blocks',
                'block_execution_orders': [{'warmup': False, 'block_id': i, 'leaf': i * 3, 'candidate_indices': [0, 1, 2]} for i in range(16)]}

    def test_formal_artifact_recomputes_comparisons_and_preserves_scope(self):
        result = analysis.analyze_run(self.fixture(), seed=1)
        self.assertEqual(result['run_class'], 'formal_candidate_selection')
        self.assertEqual(result['selection']['selected'], 'bundle3')
        self.assertEqual(result['fastest_eligible_measured_h1'], 'generic-h1')
        self.assertEqual(result['comparisons']['bundle3']['legacy-g32']['speedup'], 2.)

    def test_sample_serialization_order_does_not_change_pairing(self):
        data = self.fixture()
        data['candidates'][1]['samples'].reverse()
        result = analysis.analyze_run(data, seed=1)
        self.assertEqual(result['comparisons']['bundle3']['legacy-g32']['speedup'], 2.)

    def test_mismatched_leaf_schedule_cannot_be_labeled_paired(self):
        data = self.fixture()
        data['candidates'][1]['samples'][0]['leaf'] = 99
        with self.assertRaises(ValueError):
            analysis.normalize_run(data)

    def test_duplicate_candidate_names_cannot_overwrite_comparisons(self):
        data = self.fixture()
        data['candidates'][1]['label'] = data['candidates'][2]['label']
        with self.assertRaisesRegex(ValueError, 'labels must be unique'):
            analysis.analyze_run(data, seed=1)

    def test_candidate_batches_cannot_claim_paired_confidence(self):
        data = self.fixture()
        data['measurement_schedule'] = 'candidate_batches'
        with self.assertRaisesRegex(ValueError, 'shared interleaved'):
            analysis.analyze_run(data, seed=1)

    def test_insufficient_warmups_are_smoke_only(self):
        data = self.fixture()
        data['warmup_count'] = 1
        self.assertEqual(analysis.analyze_run(data, seed=1)['run_class'], 'smoke_only')

    def test_ineligible_faster_row_cannot_win(self):
        data = self.fixture()
        data['candidates'][2]['eligible'] = False
        self.assertEqual(analysis.analyze_run(data, seed=1)['selection']['selected'], 'generic-h1')


if __name__ == '__main__':
    unittest.main()
