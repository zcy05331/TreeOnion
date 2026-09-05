#!/usr/bin/env python3
"""Analyze recorded super-record samples; uses only the Python standard library."""

import argparse
import json
import math
import pathlib
import random
import statistics


def summarize(values):
    values = list(values)
    if not values or any(not math.isfinite(x) for x in values):
        raise ValueError('statistics require nonempty finite samples')
    ordered = sorted(values)
    return {
        'count': len(values),
        'mean': statistics.mean(values),
        'median': statistics.median(values),
        'p95': ordered[math.ceil(0.95 * len(values)) - 1],
        'stddev': statistics.stdev(values) if len(values) > 1 else 0.0,
    }


def paired_bootstrap(candidate, control, *, seed, resamples=10000):
    """Ratio of arithmetic means, resampling the same block indices in both rows."""
    candidate, control = list(candidate), list(control)
    if not candidate or len(candidate) != len(control):
        raise ValueError('paired comparisons require equal nonempty sample counts')
    if any(not math.isfinite(x) or x <= 0 for x in candidate + control):
        raise ValueError('server times must be finite and positive')
    if resamples < 10000:
        raise ValueError('the design requires at least 10000 bootstrap resamples')
    n = len(candidate)
    rng = random.Random(seed)
    ratios = []
    for _ in range(resamples):
        indices = [rng.randrange(n) for _ in range(n)]
        ratios.append(sum(candidate[i] for i in indices) / sum(control[i] for i in indices))
    ratios.sort()
    ratio = statistics.mean(candidate) / statistics.mean(control)
    lower, upper = ratios[int(0.025 * resamples)], ratios[math.ceil(0.975 * resamples) - 1]
    return {
        'candidate_over_control': ratio,
        'ratio_ci95': [lower, upper],
        'speedup': 1.0 / ratio,
        'speedup_ci95': [1.0 / upper, 1.0 / lower],
        'server_time_reduction_percent': (1.0 - ratio) * 100.0,
        'reduction_percent_ci95': [(1.0 - upper) * 100.0, (1.0 - lower) * 100.0],
        'statistically_unresolved': lower <= 1.0 <= upper,
        'bootstrap_seed': seed,
        'bootstrap_resamples': resamples,
        'paired_blocks': n,
        'estimator': 'ratio of arithmetic means; percentile paired-block bootstrap',
    }


def select_winner(rows, *, seed, resamples=10000):
    if not rows:
        return {'selected': None, 'reason': 'no eligible measured generic candidates'}
    ordered = sorted(rows, key=lambda r: (statistics.mean(r['server_ms']), r['label']))
    fastest = ordered[0]
    best_mean = statistics.mean(fastest['server_ms'])
    unresolved = [fastest]
    comparisons = {}
    for row in ordered[1:]:
        comparison = paired_bootstrap(row['server_ms'], fastest['server_ms'],
                                      seed=seed, resamples=resamples)
        within_two_percent = statistics.mean(row['server_ms']) <= best_mean * 1.02
        comparison['within_two_percent_of_fastest_mean'] = within_two_percent
        comparisons[row['label']] = comparison
        if within_two_percent or comparison['statistically_unresolved']:
            unresolved.append(row)
    chosen = fastest
    reason = 'smallest arithmetic mean; timing comparison resolved'
    if len(unresolved) > 1:
        minimum_storage = min(row['steady_state_bytes'] for row in unresolved)
        storage_tied = [row for row in unresolved
                        if row['steady_state_bytes'] <= minimum_storage * 1.02]
        chosen = min(storage_tied, key=lambda r: (r['response_count'],
                                                 statistics.mean(r['server_ms']), r['label']))
        reason = 'statistically unresolved or within 2%; storage tie-break applied'
        if len(storage_tied) > 1:
            reason += '; storage within 2%, response count tie-break applied'
    return {
        'selected': chosen['label'],
        'fastest_mean': fastest['label'],
        'unresolved_candidates': [row['label'] for row in unresolved],
        'reason': reason,
        'comparisons_to_fastest_mean': comparisons,
        'scope': 'eligible measured generic candidates under the recorded policy and hardware',
    }


def normalize_run(data):
    """Validate pairing before treating repeated leaf measurements as paired blocks."""
    if data.get('measurement_schedule') != 'interleaved_blocks':
        raise ValueError('paired analysis requires shared interleaved measurement blocks')
    labels = [r['label'] for r in data['candidates']]
    if len(labels) != len(set(labels)):
        raise ValueError('candidate labels must be unique for unambiguous comparisons')
    blocks = [b for b in data.get('block_execution_orders', []) if not b['warmup']]
    rows = []
    schedule = None
    for row_index, row in enumerate(data['candidates']):
        if not row.get('eligible', False):
            continue
        samples = sorted(row.get('samples', []), key=lambda s: (s['block_id'], s['leaf']))
        if not samples:
            continue
        keys = [(s['block_id'], s['leaf']) for s in samples]
        declared = sorted((b['block_id'], b['leaf']) for b in blocks if row_index in b['candidate_indices'])
        if keys != declared:
            raise ValueError('samples do not match the recorded shared block execution orders')
        if len(keys) != len(set(keys)):
            raise ValueError('duplicate measured block identifiers in ' + row['label'])
        if schedule is None:
            schedule = keys
        elif schedule != keys:
            raise ValueError('candidate leaf/block schedules differ; comparison is not paired')
        times = [s['server_total_ms'] for s in samples]
        summarize(times)
        if any(x <= 0 for x in times):
            raise ValueError('nonpositive server total in ' + row['label'])
        stages = {}
        for sample in samples:
            for name, value in sample['stages_ms'].items():
                if value is not None:
                    if not math.isfinite(value) or value < 0:
                        raise ValueError('invalid stage time in ' + row['label'])
                    stages.setdefault(name, []).append(value)
        stage_stats = {name: summarize(values) for name, values in stages.items()}
        shape = row.get('shape') or (row.get('candidate') or {}).get('shape') or {}
        metrics = row.get('candidate_metrics') or row.get('metrics') or (row.get('candidate') or {}).get('metrics') or {}
        is_legacy = row.get('legacy', row['label'].startswith('legacy'))
        steady_bytes = metrics.get('optimized_steady_state_bytes_estimate')
        if not is_legacy and steady_bytes is None:
            raise ValueError('generic candidate lacks the steady-state storage estimate')
        rows.append({
            'label': row['label'], 'server_ms': times, 'server_stats': summarize(times),
            'stage_stats': stage_stats, 'legacy': is_legacy,
            'h1': shape.get('bundle_arity') == 1,
            'steady_state_bytes': steady_bytes,
            'response_count': shape.get('response_ciphertexts'),
            'shape': shape, 'source_row': row,
        })
    return rows


def analyze_run(data, *, seed, resamples=10000):
    rows = normalize_run(data)
    generic = [r for r in rows if not r['legacy']]
    controls = [r for r in rows if r['legacy']]
    h1 = [r for r in generic if r['h1']]
    if h1:
        controls.append(min(h1, key=lambda r: statistics.mean(r['server_ms'])))
    comparisons = {}
    for row in generic:
        comparisons[row['label']] = {
            control['label']: paired_bootstrap(row['server_ms'], control['server_ms'],
                                                seed=seed, resamples=resamples)
            for control in controls if control['label'] != row['label']
        }
    warmups = data.get('warmup_count', 0)
    n = min((len(row['server_ms']) for row in rows), default=0)
    result = {
        'schema_version': 'superrecord-analysis-v1',
        'run_class': 'formal_candidate_selection' if warmups >= 3 and n >= 16 else 'smoke_only',
        'warmup_count': warmups, 'measured_blocks': n,
        'bootstrap_seed': seed, 'bootstrap_resamples': resamples,
        'comparison_scope': 'same leaf per shared measurement block; randomized candidate order within every block; OS drift remains',
        'fastest_eligible_measured_h1': min(h1, key=lambda r: statistics.mean(r['server_ms']))['label'] if h1 else None,
        'selection': select_winner(generic, seed=seed, resamples=resamples),
        'candidates': [{k: v for k, v in row.items() if k not in ['source_row', 'server_ms']}
                       for row in rows],
        'comparisons': comparisons,
    }
    if result['run_class'] == 'smoke_only':
        result['selection']['scope'] = 'smoke diagnostic only; insufficient trials or warmups for formal selection'
    return result


def markdown_report(result, raw_name):
    lines = ['# Generic super-record local comparison', '',
             f"Run class: `{result['run_class']}`. {result['measured_blocks']} measured blocks, "
             f"{result['warmup_count']} warmups. Raw data: [{raw_name}]({raw_name}).", '',
             '| Candidate | h / G / P; b; N0 | Mean ms | Median ms | p95 ms | SD ms | Estimated steady-state MiB |',
             '|---|---:|---:|---:|---:|---:|---:|']
    for row in result['candidates']:
        stats, shape = row['server_stats'], row['shape']
        layout = f"{shape.get('bundle_arity', '—')} / {shape.get('record_coeffs', '—')} / {shape.get('plane_count', '—')}; {shape.get('beta_bits', '—')}; {shape.get('N0', '—')}"
        memory = f"{row['steady_state_bytes'] / 1024 ** 2:.2f}" if row['steady_state_bytes'] is not None else 'unavailable'
        lines.append(f"| {row['label']} | {layout} | {stats['mean']:.3f} | {stats['median']:.3f} | "
                     f"{stats['p95']:.3f} | {stats['stddev']:.3f} | {memory} |")
    lines += ['', '| Candidate | Control | Speedup | Paired 95% interval | Time reduction | Timing resolved |',
              '|---|---|---:|---:|---:|---|']
    for label, controls in result['comparisons'].items():
        for control, comparison in controls.items():
            low, high = comparison['speedup_ci95']
            lines.append(f"| {label} | {control} | {comparison['speedup']:.3f}× | {low:.3f}–{high:.3f}× | "
                         f"{comparison['server_time_reduction_percent']:.2f}% | "
                         f"{'no' if comparison['statistically_unresolved'] else 'yes'} |")
    choice = result['selection']
    lines += ['', f"Selected measured plan: **{choice['selected']}**. {choice['reason']}.", '',
              '## Limitations', '',
              result['comparison_scope'] + '. Only eligible measured candidates are ranked. '
              'Coefficient storage estimates are separate from measured process peak RSS. '
              'Query sizes are modeled; response sizes count serialized small-q ciphertext payload only.', '']
    return '\n'.join(lines)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('run', type=pathlib.Path)
    parser.add_argument('--seed', type=int, default=90304)
    parser.add_argument('--resamples', type=int, default=10000)
    args = parser.parse_args()
    data = json.loads(args.run.read_text())
    result = analyze_run(data, seed=args.seed, resamples=args.resamples)
    out = args.run.with_name(args.run.stem + '-analysis.json')
    out.write_text(json.dumps(result, indent=2, allow_nan=False) + '\n')
    report = args.run.with_name(args.run.stem + '-analysis.md')
    report.write_text(markdown_report(result, args.run.name))
    print(f"Analysis: {report}; selected {result['selection']['selected']} ({result['run_class']})")


if __name__ == '__main__':
    main()
