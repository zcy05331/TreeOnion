"""Validate a complete path benchmark before aggregating its measured samples."""
import math
import re
from statistics import fmean

from .workload import PATH_MODE, RECORD_BYTES


def require(condition, message):
    if not condition:
        raise ValueError(message)


def nonnegative_number(value, field):
    require(isinstance(value, (float, int)) and not isinstance(value, bool)
            and math.isfinite(value) and value >= 0, f'invalid {field}')


def validate_geometry(data, height, warmups, trials):
    expected = {'schema_version': 'path-baseline-v1', 'path_mode': PATH_MODE,
                'height': height, 'leaf_count': 1 << height,
                'record_bytes': RECORD_BYTES,
                'logical_records': (1 << (height + 1)) - 1,
                'logical_db_bytes': ((1 << (height + 1)) - 1) * RECORD_BYTES,
                'warmups': warmups, 'trials': trials}
    for key, value in expected.items():
        require(data.get(key) == value, f'{key}: expected {value}, got {data.get(key)}')
    require(isinstance(data.get('backend_records'), int)
            and data['backend_records'] >= data['logical_records'], 'invalid backend_records')
    require(isinstance(data.get('batch_size'), int) and data['batch_size'] > 0, 'invalid batch_size')
    require(bool(data.get('scheme')), 'missing scheme')
    require(isinstance(data.get('source_commit'), str)
            and re.fullmatch(r'[0-9a-f]{40}', data['source_commit']) is not None,
            'source_commit must be a full verified upstream Git revision')
    require(isinstance(data.get('parameters'), dict) and data['parameters'], 'missing parameters')
    require(isinstance(data.get('setup'), dict), 'missing setup measurements')
    labels = data.get('communication', {})
    require(all(isinstance(labels.get(k), str) and labels[k] for k in ('query', 'response')),
            'missing communication measurement labels')
    require(data.get('profiling_coverage') in ('exclusive_stages', 'server_total_only'),
            'missing profiling coverage')


def validate_sample(sample, leaf, height, coverage):
    require(sample.get('leaf') == leaf, 'sample leaf differs from requested schedule')
    require(sample.get('correct') is True and sample.get('correct_entries') == height + 1
            and sample.get('path_entries') == height + 1, 'full path correctness did not pass')
    for key in ('server_ms', 'query_ms', 'decode_ms', 'query_bytes', 'response_bytes'):
        nonnegative_number(sample.get(key), key)
    require(sample['server_ms'] > 0, 'server timing must be positive')
    require(sample['query_bytes'] > 0 and sample['response_bytes'] > 0,
            'full path communication must be positive')
    stages = sample.get('stages_ms')
    require(isinstance(stages, dict), 'missing stages_ms')
    if coverage == 'exclusive_stages':
        require(bool(stages), 'exclusive stage profile is empty')
    for name, value in stages.items():
        nonnegative_number(value, 'stages_ms.' + name)
    require(sum(stages.values()) <= sample['server_ms'] + 0.02,
            'exclusive stage timings exceed complete server time')


def validate_result(data, *, height, leaves, warmups, trials):
    validate_geometry(data, height, warmups, trials)
    require(len(leaves) >= warmups + trials, 'schedule is too short')
    for key, count, offset in [('warmup_samples', warmups, 0), ('samples', trials, warmups)]:
        samples = data.get(key)
        require(isinstance(samples, list) and len(samples) == count, f'incomplete {key}')
        for index, sample in enumerate(samples):
            validate_sample(sample, leaves[offset + index], height, data['profiling_coverage'])
    stage_names = set(data['samples'][0]['stages_ms'])
    require(all(set(x['stages_ms']) == stage_names for x in data['samples']),
            'stage profile differs between measured samples')
    return data


def summarize_result(data):
    samples = data['samples']
    server_ms = fmean(x['server_ms'] for x in samples)
    return {'scheme': data['scheme'], 'height': data['height'],
            'completed_samples': len(samples), 'server_ms': server_ms,
            'logical_mib_s': data['logical_db_bytes'] / 2**20 * 1000 / server_ms,
            'query_ms': fmean(x['query_ms'] for x in samples),
            'decode_ms': fmean(x['decode_ms'] for x in samples),
            'query_bytes': fmean(x['query_bytes'] for x in samples),
            'response_bytes': fmean(x['response_bytes'] for x in samples),
            'communication': data['communication'], 'setup': data['setup'],
            'parameters': data['parameters'],
            'profiling_coverage': data['profiling_coverage'],
            'stages_ms': {name: fmean(x['stages_ms'][name] for x in samples)
                          for name in samples[0]['stages_ms']}}
