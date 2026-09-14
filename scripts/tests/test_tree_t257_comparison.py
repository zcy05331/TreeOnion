import contextlib
import io
import json
import pathlib
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
import run_tree_t257_comparison as t257
from run_tree_t257_comparison import validate_result, compare_results
from summarize_cloudlab_profiles import ProfileError


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

    def test_correct_decryption_does_not_require_noise_headroom(self):
        for budget in ('0', '-1'):
            raw = result(257)
            raw['metadata']['min_noise_budget_bits'] = budget
            validate_result(raw, pathlib.Path('candidate.json'), 257, 4096, 2, 1)

    def test_decryption_error_is_still_rejected(self):
        raw = result(257)
        raw['samples'][0]['correct'] = False
        with self.assertRaisesRegex(ProfileError, 'correctness'):
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


COMMUNICATION = {'modeled_query_bytes_per_trial': 14880, 'actual_response_bytes_per_trial': 11264,
                 'modeled_helper_key_bytes': 1607040, 'modeled_compression_key_bytes': 0,
                 'query_bytes_kind': 'modeled', 'response_bytes_kind': 'actual',
                 'helper_key_bytes_kind': 'modeled', 'compression_key_bytes_kind': 'modeled'}


def benchmark_output(t):
    """What tree_bench_g32 writes to BENCHMARK_OUTPUT_JSON (tree_profile_json::write_report):
    result() plus the top-level and communication fields that main's REPORT.md reads."""
    raw = result(t)
    raw.update({'scheme': 'legacy-tree-g32', 'actual_logical_bytes_scope': 'full_tree_all_nodes',
                'trial_seed': 7, 'offline_setup_ms': 12.5, 'mode': 'mvp',
                'diagnostic_non_comparable_32B_payload': False,
                'communication': dict(COMMUNICATION)})
    for sample in raw['samples']:
        # write_report emits these on every sample and takes the communication response size
        # from the last one.
        sample['actual_response_bytes'] = COMMUNICATION['actual_response_bytes_per_trial']
        sample['diagnostics_ms'] = {'diagnostic_scan_combined': sample['stages_ms']['scan_core']}
    raw['metadata']['min_noise_budget_bits'] = '2' if t == 4099 else '1'
    return raw


class FakeG32Benchmark:
    """Stands in for read_process_peak_memory.run_measured: records the launch, writes the log
    and the raw JSON of whichever build was invoked, and returns a run_measured-shaped record."""

    def __init__(self, builds, fail_t=None):
        self.builds = builds  # resolved binary path -> plaintext modulus of that build
        self.fail_t = fail_t
        self.calls = []

    def __call__(self, command, *, log_path, env=None, cwd=None):
        self.calls.append({'command': list(command), 'env': dict(env), 'log_path': pathlib.Path(log_path)})
        pathlib.Path(log_path).write_text('fake tree_bench_g32\n')
        t = self.builds[command[0]]
        process = {'exit_code': 0, 'elapsed_seconds': 0.1, 'process_peak_rss_bytes': 1 << 20, 'ru_maxrss_unit': 'bytes'}
        if t == self.fail_t:
            process['exit_code'] = 1
        else:
            pathlib.Path(env['BENCHMARK_OUTPUT_JSON']).write_text(json.dumps(benchmark_output(t), indent=1) + '\n')
        return process


class MainTests(unittest.TestCase):
    def setUp(self):
        tmp = tempfile.TemporaryDirectory()
        self.addCleanup(tmp.cleanup)
        root = pathlib.Path(tmp.name).resolve()
        self.baseline = root / 'build-baseline' / 'Onion-PIR'
        self.candidate = root / 'build-t257' / 'Onion-PIR'
        for binary in (self.baseline, self.candidate):
            binary.parent.mkdir()
            binary.write_text('')
        self.out = root / 'smoke'
        self.builds = {str(self.baseline): 4099, str(self.candidate): 257}

    def run_main(self, fake):
        # The smoke invocation in scripts/run_tree_t257_after_entries.sh.
        argv = ['run_tree_t257_comparison.py', '--baseline-binary', str(self.baseline),
                '--candidate-binary', str(self.candidate), '--leaf-count', '4096',
                '--experiments', '2', '--warmup', '1', '--output-dir', str(self.out)]
        with patch.object(sys, 'argv', argv), patch.object(t257, 'run_measured', fake), \
             contextlib.redirect_stdout(io.StringIO()):
            t257.main()

    def command(self, binary):
        return [str(binary), '--test', 'tree_bench_g32', '--leaf-count', '4096',
                '--experiments', '2', '--warmup', '1', '--trial-seed', '7']

    def manifest(self):
        return json.loads((self.out / 'manifest.json').read_text())

    def test_baseline_then_candidate_run_sequentially_into_per_modulus_files(self):
        fake = FakeG32Benchmark(self.builds)
        self.run_main(fake)

        self.assertEqual([call['command'] for call in fake.calls],
                         [self.command(self.baseline), self.command(self.candidate)])
        self.assertEqual([call['env']['BENCHMARK_OUTPUT_JSON'] for call in fake.calls],
                         [str(self.out / 't4099.json'), str(self.out / 't257.json')])
        self.assertEqual([call['log_path'] for call in fake.calls], [self.out / 't4099.log', self.out / 't257.log'])
        manifest = self.manifest()
        self.assertEqual(manifest['state'], 'complete')
        self.assertEqual((manifest['leaf_count'], manifest['trials'], manifest['warmups']), (4096, 2, 1))
        self.assertEqual([(run['t'], run['state']) for run in manifest['runs']], [(4099, 'complete'), (257, 'complete')])
        self.assertEqual(manifest['runs'][0]['command'], self.command(self.baseline))
        for t in (4099, 257):
            self.assertEqual(json.loads((self.out / f't{t}-process.json').read_text())['exit_code'], 0)
        comparison = json.loads((self.out / 'comparison.json').read_text())
        self.assertEqual(comparison['speedup'], 1.25)
        self.assertEqual(comparison['baseline_communication'], COMMUNICATION)
        self.assertEqual(comparison['stage_rows'][0]['stage'], 'scan_core')

    def test_report_rows_read_the_communication_and_noise_fields_the_benchmark_writes(self):
        # The unit tests above compare with communication == {}; REPORT.md indexes the
        # communication keys and min_noise_budget_bits, which on the cluster are first read
        # after both formal runs have finished.
        self.run_main(FakeG32Benchmark(self.builds))
        report = (self.out / 'REPORT.md').read_text().splitlines()
        # (2 * 4096 - 1) * 32 B = 0.249969 MiB over 0.100 s and 0.080 s.
        self.assertIn('| t4099 / c12 | 100.000 | 2.500 |', report)
        self.assertIn('| t257 / c8 | 80.000 | 3.125 |', report)
        self.assertIn('| scan_core | 60.000 | 40.000 | 20.000 |', report)
        self.assertIn('| t=4099 | 14880 | 11264 | 1607040 | 2 |', report)
        self.assertIn('| t=257 | 14880 | 11264 | 1607040 | 1 |', report)

    def test_failed_baseline_marks_the_run_failed_and_never_launches_the_candidate(self):
        fake = FakeG32Benchmark(self.builds, fail_t=4099)
        with self.assertRaisesRegex(ValueError, 't=4099 failed'):
            self.run_main(fake)
        self.assertEqual(len(fake.calls), 1)
        manifest = self.manifest()
        self.assertEqual(manifest['state'], 'failed')
        self.assertIn('t=4099 failed', manifest['error'])
        self.assertEqual([(run['t'], run['state']) for run in manifest['runs']], [(4099, 'failed')])
        self.assertEqual(json.loads((self.out / 't4099-process.json').read_text())['exit_code'], 1)
        for name in ('t257-process.json', 't257.json', 't257.log', 'comparison.json', 'REPORT.md'):
            self.assertFalse((self.out / name).exists(), name)

    def test_existing_run_output_is_never_overwritten(self):
        # Re-invoking run_tree_t257_after_entries.sh with the same --output-dir after a
        # partial comparison must stop before launching, leaving the measured file intact.
        self.out.mkdir(parents=True)
        (self.out / 't4099.json').write_text('{}\n')
        fake = FakeG32Benchmark(self.builds)
        with self.assertRaisesRegex(ValueError, 'existing run output'):
            self.run_main(fake)
        self.assertEqual(fake.calls, [])
        self.assertEqual((self.out / 't4099.json').read_text(), '{}\n')
        self.assertEqual(self.manifest()['state'], 'failed')


if __name__ == '__main__':
    unittest.main()
