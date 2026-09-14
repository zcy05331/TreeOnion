import contextlib
import importlib.util
import io
import json
import pathlib
import statistics
import sys
import tempfile
import unittest
from unittest.mock import patch

SCRIPTS = pathlib.Path(__file__).resolve().parents[1]
MODULE = SCRIPTS / 'run_entry_size_sweep.py'
sys.path.insert(0, str(SCRIPTS))
spec = importlib.util.spec_from_file_location('entry_size_sweep', MODULE)
sweep = importlib.util.module_from_spec(spec)
spec.loader.exec_module(sweep)
import analyze_superrecord_sweep as analyzer  # noqa: E402  (the module sweep.analyze_run lives in)


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


def raw_doc(entry_bits=256, leaf_count=1 << 24, measured_trials=100, warmups=3, *, only_h1=False, extra_arity=None, subset=None, candidates=None):
    if candidates is None:
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


def timed_candidate(label, arity, server_ms):
    """candidate() with the server times replaced, so the report statistics are exact.
    A scalar pins every sample; a list is used as given."""
    row = candidate(label, arity)
    values = [server_ms] * len(row['samples']) if not isinstance(server_ms, list) else server_ms
    for sample, value in zip(row['samples'], values):
        sample['server_total_ms'] = value
    return row


def three_way_candidates():
    """Two h3 rows and a faster h1. The default fixture has one row per arity and a slower
    h1, so best_overall == best_h3 and the min over several h3 rows are never exercised.
    The slower h3 is listed first, so selecting the first h3 row instead of the min-mean one
    changes the answer. The h1 times are skewed (mean 900.0, median 800.0) so the report's
    throughput column cannot pass by using the median."""
    return [timed_candidate('h1', 1, [800.0] * 90 + [1800.0] * 10),
            timed_candidate('bundle3-slow', 3, 1300.0),
            timed_candidate('bundle3', 3, 1000.0)]


def write_source(tmp, data, exit_code=0):
    """A reusable 32B source the way the matrix leaves it: generic.json next to generic-process.json."""
    raw = pathlib.Path(tmp) / 'generic.json'
    raw.write_text(json.dumps(data) + '\n')
    raw.with_name('generic-process.json').write_text(json.dumps({'exit_code': exit_code}) + '\n')
    return raw


def report_table(report_text):
    """The REPORT.md selection table as one dict per data row, keyed by header cell."""
    lines = [line for line in report_text.splitlines() if line.startswith('| ')]
    header = [cell.strip() for cell in lines[0].strip('|').split('|')]
    return [dict(zip(header, (cell.strip() for cell in line.strip('|').split('|')))) for line in lines[2:]]


def resolved_comparison(candidate_ms, control_ms, *, seed, resamples=10000):
    """Stands in for analyze_superrecord_sweep.paired_bootstrap with a degenerate interval.
    Selection and the report read only the per-row statistics; the real bootstrap is covered
    by test_analyze_superrecord_sweep and costs about a second per comparison at 100 samples."""
    ratio = statistics.mean(candidate_ms) / statistics.mean(control_ms)
    return {'candidate_over_control': ratio, 'ratio_ci95': [ratio, ratio], 'speedup': 1.0 / ratio,
            'speedup_ci95': [1.0 / ratio, 1.0 / ratio], 'server_time_reduction_percent': (1.0 - ratio) * 100.0,
            'reduction_percent_ci95': [(1.0 - ratio) * 100.0] * 2, 'statistically_unresolved': ratio == 1.0,
            'bootstrap_seed': seed, 'bootstrap_resamples': resamples, 'paired_blocks': len(candidate_ms),
            'estimator': 'test stub: ratio of arithmetic means without resampling'}


class EntryPlanTests(unittest.TestCase):
    def test_48b_reuses_32b_leaves_and_reports_larger_capacity(self):
        plan = sweep.entry_plan(48)
        self.assertEqual(plan['leaf_count'], sweep.entry_plan(32)['leaf_count'])
        self.assertEqual(plan['entry_bits'], 384)
        self.assertEqual(plan['actual_tree_bytes'], 1610612688)
        self.assertEqual(plan['logical_db_bytes'], 3 * 2 ** 29)
        self.assertEqual(plan['max_responses'], 2)

    def test_requested_memory_budget_reaches_manifest(self):
        with patch.dict(sweep.os.environ, {'SUPERRECORD_MEMORY_BYTES': str(80 * 2 ** 30)}):
            manifest = sweep.build_manifest(pathlib.Path('/tmp/entry-plan'), [16], None, plan_only=True)
        self.assertEqual(manifest['cases'][0]['environment']['SUPERRECORD_MEMORY_BYTES'], str(80 * 2 ** 30))

    def test_default_matrix_matches_updated_entry_request(self):
        self.assertEqual(sweep.DEFAULT_ENTRY_BYTES, (16, 48, 64, 128))

    def test_failed_entry_does_not_skip_independent_sizes(self):
        with tempfile.TemporaryDirectory() as tmp:
            with patch.object(sweep, 'run_entry', side_effect=sweep.EntrySweepError('benchmark failed')) as run:
                code = sweep.main(['--binary', str(MODULE), '--output-dir', tmp])
            manifest = json.loads((pathlib.Path(tmp) / 'manifest.json').read_text())
        self.assertEqual(code, 1)
        self.assertEqual(run.call_count, 4)
        self.assertTrue(all(case['state'] == 'failed' for case in manifest['cases']))

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
        self.assertEqual(first_env['SUPERRECORD_MIN_NOISE_MARGIN'], '0')
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


class SelectionReportTests(unittest.TestCase):
    def setUp(self):
        patcher = patch.object(analyzer, 'paired_bootstrap', resolved_comparison)
        patcher.start()
        self.addCleanup(patcher.stop)

    def reuse(self, tmp, data, plan):
        out = pathlib.Path(tmp) / 'out'
        result = sweep.reuse_entry(out, write_source(tmp, data), plan)
        raw = json.loads(pathlib.Path(result['raw_file']).read_text())
        return sweep.mean_first_selection(raw, result['analysis']), (out / 'REPORT.md').read_text()

    def test_selection_is_min_mean_per_arity_and_fastest_row_overall(self):
        with tempfile.TemporaryDirectory() as tmp:
            selection, _ = self.reuse(tmp, raw_doc(candidates=three_way_candidates()), sweep.entry_plan(32))
        # best_overall spans both arities (the h1 here beats every h3); best_h3 is the min-mean h3.
        self.assertEqual(selection['best_overall'], {'label': 'h1', 'mean_ms': 900.0})
        self.assertEqual(selection['best_h3'], {'label': 'bundle3', 'mean_ms': 1000.0})
        self.assertEqual(selection['best_h1'], {'label': 'h1', 'mean_ms': 900.0})

    def test_report_throughput_divides_full_tree_bytes_by_each_rows_mean_server_time(self):
        with tempfile.TemporaryDirectory() as tmp:
            _, report = self.reuse(tmp, raw_doc(candidates=three_way_candidates()), sweep.entry_plan(32))
        self.assertIn('Logical DB throughput uses actual full-tree bytes divided by mean server time.', report)
        # The two capacities the legend distinguishes: the padded 1 GiB target and the tree
        # the run actually holds. Three decimals of MiB/s cannot separate them at 2^24 leaves,
        # so this is the only place the report states which one it means.
        self.assertIn('Logical DB bytes: 1073741824.', report)
        self.assertIn('Actual full tree bytes: 1073741792.', report)
        h3, h1 = report_table(report)
        self.assertEqual((h3['category'], h3['candidate']), ('best h3', 'bundle3'))
        self.assertEqual((h1['category'], h1['candidate']), ('best h1', 'h1'))
        # 1073741792 B, all 2^25 - 1 nodes, over 1.000 s is 1024.000 MiB/s.
        self.assertEqual((h3['mean ms'], h3['logical DB MiB/s']), ('1000.000', '1024.000'))
        # Each row divides by its own mean, not by the median (800.000 here) and not by the
        # winner's: 1073741792 B over 0.900 s is 1137.778 MiB/s.
        self.assertEqual((h1['mean ms'], h1['median ms']), ('900.000', '800.000'))
        self.assertEqual(h1['logical DB MiB/s'], '1137.778')
        # Wire and key columns are taken from the raw row carrying the selected label.
        self.assertEqual((h3['query modeled B'], h3['response actual B'], h3['once keys modeled B']),
                         ('14880', '11264', '1607040'))

    def test_48b_report_uses_the_1_5_gib_tree_bytes(self):
        with tempfile.TemporaryDirectory() as tmp:
            _, report = self.reuse(tmp, raw_doc(entry_bits=384, candidates=three_way_candidates()), sweep.entry_plan(48))
        h3, _ = report_table(report)
        # (2 * 2^24 - 1) * 48 B = 1610612688 B over 1.000 s: the 48B plan keeps the 32B leaf
        # count, so its throughput row must grow with the entry size, not stay at 1 GiB.
        self.assertEqual(h3['logical DB MiB/s'], '1536.000')
        self.assertIn('Actual full tree bytes: 1610612688.', report)


class MainReuseTests(unittest.TestCase):
    def setUp(self):
        patcher = patch.object(analyzer, 'paired_bootstrap', resolved_comparison)
        patcher.start()
        self.addCleanup(patcher.stop)

    def test_reuse_32b_completes_without_a_binary_and_records_the_selection(self):
        # run_entry_size_after_matrix.sh: --reuse-32b <matrix generic.json> with no new 32B run.
        with tempfile.TemporaryDirectory() as tmp:
            raw = write_source(tmp, raw_doc())
            out = pathlib.Path(tmp) / 'out'
            with contextlib.redirect_stdout(io.StringIO()):
                code = sweep.main(['--entries', '32', '--reuse-32b', str(raw), '--output-dir', str(out)])
            # main resolves --output-dir, so the manifest paths are the resolved ones.
            manifest = json.loads((out / 'manifest.json').read_text())
            entry_dir = out.resolve() / 'entry_32B'
            for name in ('raw.json', 'raw-process.json', 'analysis.json', 'REPORT.md'):
                self.assertTrue((entry_dir / name).exists(), name)
            # No log next to the source: the entry records where the original log lives instead.
            self.assertEqual((entry_dir / 'log-source.txt').read_text(), str(raw.resolve().with_suffix('.log')) + '\n')
        self.assertEqual(code, 0)
        self.assertEqual(manifest['state'], 'complete')
        case, = manifest['cases']
        self.assertEqual(case['state'], 'reused')
        self.assertEqual(case['source_raw_file'], str(raw.resolve()))
        self.assertEqual(case['raw_file'], str(entry_dir / 'raw.json'))
        self.assertIsNone(case['log_file'])
        self.assertEqual(case['best_overall']['label'], 'bundle3')
        self.assertEqual(case['best_h1']['label'], 'h1')
        self.assertEqual(case['best_h3']['label'], 'bundle3')
        # The analysis body stays in analysis.json; the manifest carries only its path.
        self.assertNotIn('analysis', case)

    def test_reuse_of_a_failed_source_process_is_rejected_before_copying(self):
        with tempfile.TemporaryDirectory() as tmp:
            raw = write_source(tmp, raw_doc(), exit_code=1)
            out = pathlib.Path(tmp) / 'out'
            with self.assertRaisesRegex(sweep.EntrySweepError, 'exit_code is not 0'):
                sweep.reuse_entry(out, raw, sweep.entry_plan(32))
            self.assertFalse((out / 'raw.json').exists())
            self.assertFalse((out / 'analysis.json').exists())


if __name__ == '__main__':
    unittest.main()
