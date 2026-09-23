import json
import pathlib
import sys
import tempfile
import unittest
from unittest.mock import patch

sys.path.insert(0, str(pathlib.Path(__file__).resolve().parents[1]))
from path_baselines.runner import adapter_command, run_campaign, write_summary
from path_baselines.workload import trial_leaves


class PathRunnerTests(unittest.TestCase):
    def test_command_carries_the_shared_workload_arguments(self):
        command = adapter_command('respire', pathlib.Path('/bin/adapter'), 8, pathlib.Path('/tmp/leaves'), 1, 2, pathlib.Path('/tmp/result'))
        self.assertEqual(command[:3], ['/bin/adapter', '--height', '8'])
        self.assertEqual(command[-2:], ['--output', '/tmp/result'])

    def test_protocol_arguments_are_forwarded_as_separate_tokens(self):
        command = adapter_command('respire', pathlib.Path('/bin/adapter'), 8,
                                  pathlib.Path('/tmp/leaves'), 1, 2, pathlib.Path('/tmp/result'),
                                  extra_args=['--flag', 'value'])
        self.assertEqual(command[-2:], ['--flag', 'value'])

    def test_summary_preserves_actual_query_label_and_modeled_response(self):
        row = {'scheme': 'respire', 'completed_samples': 1, 'server_ms': 2.0,
               'logical_mib_s': 1.0, 'query_bytes': 128, 'response_bytes': 256,
               'communication': {'query': 'actual_serialized_query',
                                 'response': 'coefficient_payload_model'}, 'stages_ms': {}}
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            write_summary(root, {'state': 'complete', 'runs': [], 'summaries': [row]})
            table = (root / 'RESULTS.md').read_text()
            self.assertIn('128 (actual)', table)
            self.assertIn('256 (model)', table)

    def test_failed_process_is_preserved_and_never_summarized(self):
        with tempfile.TemporaryDirectory() as tmp:
            root = pathlib.Path(tmp)
            with patch('path_baselines.runner.run_measured', return_value={'exit_code': 2, 'process_peak_rss_bytes': 1024}):
                result = run_campaign({'respire': pathlib.Path(sys.executable)}, root, height=8, warmups=1, trials=2, seed=7)
            self.assertEqual(result['state'], 'complete_with_failures')
            self.assertEqual(result['runs'][0]['state'], 'failed')
            self.assertEqual(result['summaries'], [])
            self.assertTrue((root/'respire-process.json').exists())
            manifest = json.loads((root/'manifest.json').read_text())
            self.assertEqual(manifest['leaves'], trial_leaves(256, 1, 2, 7))

    def test_failed_correctness_does_not_become_successful_timing(self):
        def fake_process(command, *, log_path, env, cwd):
            output = pathlib.Path(command[command.index('--output')+1])
            output.write_text(json.dumps({'schema_version':'path-baseline-v1', 'samples':[]}))
            return {'exit_code':0,'process_peak_rss_bytes':1024}
        with tempfile.TemporaryDirectory() as tmp:
            with patch('path_baselines.runner.run_measured', side_effect=fake_process):
                result = run_campaign({'respire':pathlib.Path(sys.executable)}, pathlib.Path(tmp), height=8, warmups=1, trials=2, seed=7)
            self.assertEqual(result['runs'][0]['state'], 'failed')
            self.assertFalse(result['summaries'])


if __name__ == '__main__':
    unittest.main()
