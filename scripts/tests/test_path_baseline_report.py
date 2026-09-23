import sys
import unittest
from pathlib import Path

sys.path.insert(0, str(Path(__file__).resolve().parents[1]))
from path_baselines.report import validate_result, summarize_result


def fixture():
    sample = {'leaf': 6, 'server_ms': 2.0, 'query_ms': 0.1, 'decode_ms': 0.2,
              'query_bytes': 128, 'response_bytes': 256, 'correct': True,
              'correct_entries': 4, 'path_entries': 4, 'stages_ms': {'scan': 1.5}}
    return {'schema_version': 'path-baseline-v1', 'scheme': 'respire',
            'path_mode': 'ancestor_root_included', 'height': 3, 'leaf_count': 8,
            'record_bytes': 32, 'logical_records': 15, 'logical_db_bytes': 480,
            'backend_records': 16, 'batch_size': 4, 'warmups': 0, 'trials': 1,
            'source_commit': '0' * 40, 'parameters': {'n': 2048},
            'setup': {'preprocess_ms': 4.0},
            'communication': {'query': 'actual_serialized', 'response': 'actual_serialized'},
            'profiling_coverage': 'exclusive_stages', 'warmup_samples': [], 'samples': [sample]}


class PathReportTests(unittest.TestCase):
    def test_full_workload_and_one_path_denominator(self):
        data = fixture()
        validate_result(data, height=3, leaves=[6], warmups=0, trials=1)
        row = summarize_result(data)
        self.assertEqual(row['completed_samples'], 1)
        self.assertAlmostEqual(row['logical_mib_s'], 480 / 2**20 / .002)
        self.assertEqual(row['stages_ms']['scan'], 1.5)

    def test_wrong_path_or_wrong_count_is_not_a_completed_run(self):
        for key, value in [('leaf', 7), ('correct', False), ('correct_entries', 3), ('server_ms', float('nan'))]:
            data = fixture();data['samples'][0][key] = value
            with self.assertRaises(ValueError, msg=key):
                validate_result(data, height=3, leaves=[6], warmups=0, trials=1)
        data = fixture();data['samples'] = []
        with self.assertRaises(ValueError):
            validate_result(data, height=3, leaves=[6], warmups=0, trials=1)

    def test_root_omission_and_wrong_denominator_are_rejected(self):
        for key, value in [('logical_records', 14), ('logical_db_bytes', 1024), ('path_mode', 'nonroot_only')]:
            data = fixture();data[key] = value
            with self.assertRaises(ValueError, msg=key):
                validate_result(data, height=3, leaves=[6], warmups=0, trials=1)

    def test_overlapping_or_missing_stage_measurement_is_not_filled_with_zero(self):
        data = fixture();data['samples'][0]['stages_ms']['merge'] = 1.0
        with self.assertRaises(ValueError):
            validate_result(data, height=3, leaves=[6], warmups=0, trials=1)
        data = fixture();data['profiling_coverage'] = 'server_total_only';data['samples'][0]['stages_ms'] = {}
        validate_result(data, height=3, leaves=[6], warmups=0, trials=1)
        self.assertEqual(summarize_result(data)['stages_ms'], {})

    def test_unknown_or_short_source_revision_is_not_formal_evidence(self):
        for revision in ('unknown', '930063c', 'TreePIR/Spiral 930063c5aefc'):
            data = fixture();data['source_commit'] = revision
            with self.assertRaises(ValueError):
                validate_result(data, height=3, leaves=[6], warmups=0, trials=1)

    def test_communication_measurement_kind_is_retained(self):
        data = fixture();data['communication']['query'] = 'coefficient_payload_model'
        validate_result(data, height=3, leaves=[6], warmups=0, trials=1)
        self.assertEqual(summarize_result(data)['communication']['query'], 'coefficient_payload_model')
        data['communication'] = {}
        with self.assertRaises(ValueError):
            validate_result(data, height=3, leaves=[6], warmups=0, trials=1)


if __name__ == '__main__':
    unittest.main()
