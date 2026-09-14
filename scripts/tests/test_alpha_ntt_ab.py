import copy
import json
import pathlib
import sys
import unittest
import tempfile
from unittest.mock import patch

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from benchmark_alpha_ntt_ab import validate_h3_result
import benchmark_alpha_ntt_ab as ab
from summarize_cloudlab_profiles import ProfileError

WIRE = {'modeled_seeded_query_bytes': 100, 'actual_response_ciphertext_payload_bytes': 200}


def h3_profile(leaf_count, experiments, server_ms, n0=352):
    """The tree_superrecord_bench JSON the A/B driver reads back: one pinned h3 candidate."""
    samples = [{'block_id': i, 'leaf': 11 + i, 'server_total_ms': server_ms, 'client_query_ms': 1.,
                'client_decode_ms': 1., 'response_serialize_ms': 1.,
                'stages_ms': {'prepare': server_ms / 4, 'scan': server_ms / 2}} for i in range(experiments)]
    return {'leaf_count': leaf_count, 'entry_bits': 256, 'measured_trials': experiments, 'warmup_count': 3,
            'candidates': [{'eligible': True,
                            'shape': {'bundle_arity': 3, 'record_coeffs': 64, 'plane_count': 1, 'N0': n0,
                                      'beta_bits': leaf_count.bit_length() - 1 - 15},
                            'scheme_fingerprint': {'identity': {'ring_degree': 2048}},
                            'level_plans': [{'level': 0}], 'wire': WIRE,
                            'target_noise_result': {'decrypts_correctly': True,
                                                    'observed_key_coverage_matches_tag': True},
                            'samples': samples}]}


class AbDriverTests(unittest.TestCase):
    """main() with run_measured replaced by a fake that answers like the two binaries would."""
    OLD = str(pathlib.Path('/tmp/old').resolve())
    NEW = str(pathlib.Path('/tmp/new').resolve())

    def run_ab(self, out, leaf_count, new_n0=352):
        recorded = []
        def run_measured(command, *, log_path, env=None, cwd=None):
            recorded.append({'binary': command[0], 'env': env.copy()})
            raw = pathlib.Path(env['SUPERRECORD_OUTPUT_JSON'])
            variant = raw.stem.split('-')[1]
            profile = h3_profile(int(command[command.index('--leaf-count') + 1]),
                                 int(command[command.index('--experiments') + 1]),
                                 20. if variant == 'old' else 10.,
                                 n0=352 if variant == 'old' else new_n0)
            raw.write_text(json.dumps(profile) + '\n')
            pathlib.Path(log_path).write_text('ok\n')
            return {'exit_code': 0, 'elapsed_seconds': 0.1, 'process_peak_rss_bytes': 10}
        argv = ['benchmark', '--baseline', '/tmp/old', '--optimized', '/tmp/new',
                '--baseline-commit', 'commit-old', '--optimized-commit', 'commit-new',
                '--output', str(out), '--leaf-count', str(leaf_count), '--experiments', '2',
                '--memory-gib', '80']
        with patch.object(sys, 'argv', argv), patch.object(ab, 'run_measured', run_measured):
            ab.main()
        return recorded

    def test_abba_order_with_per_variant_pin_and_commit(self):
        # The A/B is attributable only if each run tags the binary's own commit, pins the same
        # shape (beta_bits follows the leaf count) and interleaves old/new as ABBA.
        for leaf_count, beta in ((1 << 24, 9), (1 << 22, 7)):
            with self.subTest(leaf_count=leaf_count), tempfile.TemporaryDirectory() as tmp:
                out = pathlib.Path(tmp) / 'results'
                recorded = self.run_ab(out, leaf_count)
                self.assertEqual([pathlib.Path(r['env']['SUPERRECORD_OUTPUT_JSON']).stem for r in recorded],
                                 ['round0-old', 'round0-new', 'round1-new', 'round1-old'])
                self.assertEqual([r['binary'] for r in recorded], [self.OLD, self.NEW, self.NEW, self.OLD])
                self.assertEqual([r['env']['SUPERRECORD_COMMIT'] for r in recorded],
                                 ['commit-old', 'commit-new', 'commit-new', 'commit-old'])
                self.assertEqual({r['env']['SUPERRECORD_PIN_SHAPE'] for r in recorded}, {f'3,64,1,352,{beta}'})
                manifest = json.loads((out / 'manifest.json').read_text())
                self.assertEqual(manifest['state'], 'complete')
                self.assertEqual([(r['name'], r['state'], r['source_commit']) for r in manifest['runs']],
                                 [('round0-old', 'complete', 'commit-old'), ('round0-new', 'complete', 'commit-new'),
                                  ('round1-new', 'complete', 'commit-new'), ('round1-old', 'complete', 'commit-old')])
                summary = json.loads((out / 'comparison.json').read_text())
                # Two runs of two experiments per variant; speedup is the old/new mean ratio.
                self.assertEqual((summary['old']['samples'], summary['new']['samples']), (4, 4))
                self.assertEqual((summary['old']['server_ms'], summary['new']['server_ms']), (20., 10.))
                self.assertEqual(summary['old']['stages_ms'], {'prepare': 5., 'scan': 10.})
                self.assertEqual(summary['speedup'], 2.)
                self.assertEqual(summary['communication'], WIRE)

    def test_recalibrated_layout_stops_the_comparison(self):
        # If the optimized binary calibrates a different N0 the runs are not a fixed-layout A/B:
        # stop at that run (no round 1), record the failure, write no comparison.
        with tempfile.TemporaryDirectory() as tmp:
            out = pathlib.Path(tmp) / 'results'
            with self.assertRaisesRegex(ValueError, 'calibrated layout changed'):
                self.run_ab(out, 1 << 24, new_n0=176)
            manifest = json.loads((out / 'manifest.json').read_text())
            self.assertEqual(manifest['state'], 'failed')
            self.assertIn('round0-new: calibrated layout changed', manifest['error'])
            self.assertEqual([r['name'] for r in manifest['runs']], ['round0-old', 'round0-new'])
            self.assertFalse((out / 'comparison.json').exists())


class H3ProfileTests(unittest.TestCase):
    def test_server_memory_budget_reaches_the_benchmark(self):
        captured = {}
        def capture(command, **kwargs):
            captured.update(kwargs['env'])
            raise RuntimeError('captured benchmark invocation')
        with tempfile.TemporaryDirectory() as tmp:
            argv = ['benchmark', '--baseline', '/tmp/old', '--optimized', '/tmp/new',
                    '--baseline-commit', 'old', '--optimized-commit', 'new',
                    '--output', tmp, '--leaf-count', '16777216', '--memory-gib', '80']
            with patch.object(sys, 'argv', argv), patch.object(ab, 'run_measured', capture):
                with self.assertRaisesRegex(RuntimeError, 'captured'):
                    ab.main()
        self.assertEqual(captured['SUPERRECORD_MEMORY_BYTES'], str(80 * 2**30))

    def setUp(self):
        sample = {'server_total_ms':10.,'client_query_ms':1.,'client_decode_ms':1.,
                  'response_serialize_ms':1.,'stages_ms':{'prepare':2.,'scan':8.}}
        self.data = {'leaf_count':4096,'entry_bits':256,'measured_trials':2,'warmup_count':3,
                     'candidates':[{'eligible':True,'shape':{'bundle_arity':3},
                                    'samples':[sample,copy.deepcopy(sample)],
                                    'target_noise_result':{'decrypts_correctly':True,
                                                          'observed_key_coverage_matches_tag':True}}]}

    def validate(self):
        return validate_h3_result(self.data, pathlib.Path('profile.json'), 2, 4096)

    def test_h3_only_result_does_not_require_an_h1_control(self):
        self.assertIs(self.validate(), self.data['candidates'][0])

    def test_wrong_decryption_is_rejected(self):
        self.data['candidates'][0]['target_noise_result']['decrypts_correctly'] = False
        with self.assertRaisesRegex(ValueError, 'decryption'):
            self.validate()

    def test_incomplete_measurements_are_rejected(self):
        self.data['candidates'][0]['samples'].pop()
        with self.assertRaises(ProfileError):
            self.validate()

    def test_h1_cannot_be_reported_as_h3(self):
        self.data['candidates'][0]['shape']['bundle_arity'] = 1
        with self.assertRaisesRegex(ValueError, 'h3'):
            self.validate()


if __name__ == '__main__':
    unittest.main()
