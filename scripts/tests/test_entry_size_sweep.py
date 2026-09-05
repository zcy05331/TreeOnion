import importlib.util
import json
import pathlib
import sys
import tempfile
import unittest

SCRIPTS = pathlib.Path(__file__).resolve().parents[1]
MODULE = SCRIPTS / 'run_entry_size_sweep.py'
sys.path.insert(0, str(SCRIPTS))
spec = importlib.util.spec_from_file_location('entry_size_sweep', MODULE)
sweep = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sweep)


def candidate(label, arity, factor=1.0, *, samples=100):
    rows = []
    for i in range(samples):
        rows.append({
            'block_id': i,
            'leaf': i,
            'server_total_ms': (100.0 + i / 100.0) * factor,
            'client_query_ms': 1.0,
            'client_decode_ms': 1.0,
            'response_serialize_ms': 1.0,
            'stages_ms': {'scan': (80.0 + i / 100.0) * factor, 'fold': 5.0 * factor},
        })
    return {
        'label': label,
        'eligible': True,
        'is_h1': arity == 1,
        'legacy': False,
        'shape': {'bundle_arity': arity, 'record_coeffs': 64, 'plane_count': 1, 'response_ciphertexts': 1},
        'candidate_metrics': {'optimized_steady_state_bytes_estimate': 1000, 'modeled_per_client_eval_key_bytes': 1607040},
        'wire': {'modeled_seeded_query_bytes': 14880, 'actual_response_ciphertext_payload_bytes': 11264},
        'target_noise_result': {'decrypts_correctly': True, 'observed_key_coverage_matches_tag': True},
        'samples': rows,
    }


def raw_doc(entry_bits=256, leaf_count=1 << 24, measured_trials=100, warmups=3, *, only_h1=False, extra_arity=None, subset=None):
    candidates = [candidate('h1', 1, 1.2)]
    if not only_h1:
        candidates.append(candidate('bundle3', 3, 1.0))
    if extra_arity is not None:
        candidates.append(candidate(f'bundle{extra_arity}', extra_arity, 0.8))
    data = {
        'schema_version': 'tree_superrecord_benchmark.v1',
        'repository_commit': '22adeee',
        'entry_bits': entry_bits,
        'leaf_count': leaf_count,
        'warmup_count': warmups,
        'measured_trials': measured_trials,
        'measurement_schedule': 'interleaved_blocks',
        'block_execution_orders': [
            {'warmup': False, 'block_id': i, 'leaf': i, 'candidate_indices': list(range(len(candidates)))}
            for i in range(100)
        ],
        'candidates': candidates,
    }
    if subset is not None:
        data['benchmark_bundle_arities'] = subset
    return data


class EntryPlanTests(unittest.TestCase):
    def test_empty_entry_selection_cannot_report_complete(self):
        with tempfile.TemporaryDirectory() as tmp:
            with self.assertRaises(SystemExit) as error:
                sweep.main(['--entries', '', '--output-dir', tmp])
            self.assertEqual(error.exception.code, 2)
            self.assertFalse((pathlib.Path(tmp) / 'manifest.json').exists())

    def test_leaf_counts_and_actual_tree_bytes_for_1gib(self):
        expected_leaves = {16: 1 << 25, 32: 1 << 24, 64: 1 << 23, 128: 1 << 22}
        for entry_bytes, leaves in expected_leaves.items():
            plan = sweep.entry_plan(entry_bytes)
            self.assertEqual(plan['entry_bits'], entry_bytes * 8)
            self.assertEqual(plan['leaf_count'], leaves)
            self.assertEqual(plan['actual_tree_bytes'], (2 * leaves - 1) * entry_bytes)
        self.assertEqual(sweep.entry_plan(16)['max_responses'], 1)
        self.assertEqual(sweep.entry_plan(64)['max_responses'], 2)
        self.assertEqual(sweep.entry_plan(128)['max_responses'], 4)

    def test_plan_manifest_preserves_order_knobs_and_source_commit(self):
        with tempfile.TemporaryDirectory() as tmp:
            manifest = sweep.build_manifest(pathlib.Path(tmp), [64, 16, 32, 128], pathlib.Path('/tmp/Onion-PIR'), plan_only=True, source_commit='fb79387')
        self.assertEqual([case['entry_bytes'] for case in manifest['cases']], [64, 16, 32, 128])
        self.assertEqual(manifest['binary_source_commit'], 'fb79387')
        first_env = manifest['cases'][0]['environment']
        self.assertEqual(first_env['SUPERRECORD_ENTRY_BITS'], '512')
        self.assertEqual(first_env['SUPERRECORD_SKIP_LEGACY'], '1')
        self.assertEqual(first_env['SUPERRECORD_MAX_RESPONSES'], '2')
        self.assertEqual(first_env['SUPERRECORD_MEMORY_BYTES'], str(320 * 1024 ** 3))
        self.assertEqual(first_env['SUPERRECORD_MAX_FRONTIER'], '2')
        self.assertEqual(first_env['SUPERRECORD_MAX_ARITY'], '3')
        self.assertEqual(first_env['SUPERRECORD_BUNDLE_ARITIES'], '1,3')
        self.assertEqual(first_env['SUPERRECORD_FIXED_KEY_HEIGHT'], '11')
        self.assertEqual(first_env['SUPERRECORD_MIN_NOISE_MARGIN'], '1')
        self.assertEqual(first_env['SUPERRECORD_COMMIT'], 'fb79387')


class ReuseValidationTests(unittest.TestCase):
    def write_source(self, tmp, data):
        raw = pathlib.Path(tmp) / 'generic.json'
        proc = pathlib.Path(tmp) / 'generic-process.json'
        raw.write_text(json.dumps(data) + '\n')
        proc.write_text(json.dumps({'exit_code': 0}) + '\n')
        return raw

    def test_reuse_32b_validates_entry_trials_warmups_and_process(self):
        with tempfile.TemporaryDirectory() as tmp:
            raw = self.write_source(tmp, raw_doc())
            out = pathlib.Path(tmp) / 'out' / 'entry_32B'
            result = sweep.reuse_entry(out, raw, sweep.entry_plan(32))
            self.assertTrue(pathlib.Path(result['raw_file']).exists())
            manifest_selection = sweep.mean_first_selection(
                json.loads(pathlib.Path(result['raw_file']).read_text()), result['analysis'])
            self.assertEqual(manifest_selection['best_overall']['label'], 'bundle3')
            self.assertEqual(manifest_selection['best_h1']['label'], 'h1')
            self.assertEqual(manifest_selection['best_h3']['label'], 'bundle3')

    def test_reuse_rejects_wrong_entry_bits(self):
        with tempfile.TemporaryDirectory() as tmp:
            raw = self.write_source(tmp, raw_doc(entry_bits=512, leaf_count=1 << 24))
            with self.assertRaisesRegex(sweep.EntrySweepError, 'entry_bits'):
                sweep.reuse_entry(pathlib.Path(tmp) / 'out', raw, sweep.entry_plan(32))

    def test_own_output_can_be_reused_with_process_log_and_source_intact(self):
        with tempfile.TemporaryDirectory() as tmp:
            data = raw_doc()
            data['repository_commit'] = 'abcdef012345'
            raw = self.write_source(tmp, data)
            raw.with_suffix('.log').write_text('test fixture log\n')
            first = sweep.reuse_entry(pathlib.Path(tmp) / 'first', raw, sweep.entry_plan(32))
            second_dir = pathlib.Path(tmp) / 'second'
            second = sweep.reuse_entry(second_dir, pathlib.Path(first['raw_file']), sweep.entry_plan(32))
            self.assertEqual(pathlib.Path(second['log_file']).read_text(), 'test fixture log\n')
            self.assertEqual(json.loads(pathlib.Path(second['raw_file']).read_text())['repository_commit'], 'abcdef012345')
            self.assertNotIn('22adeee', (second_dir / 'REPORT.md').read_text())

    def test_reuse_rejects_trial_mismatch(self):
        with tempfile.TemporaryDirectory() as tmp:
            data = raw_doc(measured_trials=99)
            for cand in data['candidates']:
                cand['samples'] = cand['samples'][:99]
            data['block_execution_orders'] = data['block_execution_orders'][:99]
            raw = self.write_source(tmp, data)
            with self.assertRaisesRegex(sweep.EntrySweepError, 'samples|measured_trials'):
                sweep.reuse_entry(pathlib.Path(tmp) / 'out', raw, sweep.entry_plan(32))

    def test_only_h1_does_not_count_as_complete(self):
        with tempfile.TemporaryDirectory() as tmp:
            raw = self.write_source(tmp, raw_doc(only_h1=True))
            with self.assertRaisesRegex(sweep.EntrySweepError, 'h3'):
                sweep.reuse_entry(pathlib.Path(tmp) / 'out', raw, sweep.entry_plan(32))

    def test_h4_cannot_replace_required_h3(self):
        with tempfile.TemporaryDirectory() as tmp:
            raw = self.write_source(tmp, raw_doc(only_h1=True, extra_arity=4))
            with self.assertRaisesRegex(sweep.EntrySweepError, 'expected only 1 and 3'):
                sweep.reuse_entry(pathlib.Path(tmp) / 'out', raw, sweep.entry_plan(32))

    def test_subset_field_must_be_exact_when_present(self):
        with tempfile.TemporaryDirectory() as tmp:
            raw = self.write_source(tmp, raw_doc(subset=[1, 3, 4]))
            with self.assertRaisesRegex(sweep.EntrySweepError, 'benchmark_bundle_arities'):
                sweep.reuse_entry(pathlib.Path(tmp) / 'out', raw, sweep.entry_plan(32))


if __name__ == '__main__':
    unittest.main()
