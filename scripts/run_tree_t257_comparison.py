#!/usr/bin/env python3
"""Measure matched Tree g32 builds at t=4099 and t=257, sequentially."""
import argparse
import json
import os
from pathlib import Path
from statistics import fmean

from read_process_peak_memory import run_measured
from summarize_cloudlab_profiles import validate_profile


def validate_result(raw, path, modulus, leaves, trials, warmups):
    validate_profile(raw, path, trials, leaves)
    expected = {'plaintext_modulus': modulus, 'chunk_bits': 12 if modulus == 4099 else 8,
                'ring_degree': 2048, 'g': 32, 'response_modulus': 4169729}
    for key, value in expected.items():
        if int(raw['metadata'][key]) != value:
            raise ValueError(f'{path}: unexpected {key}')
    if raw['node_entry_bits'] != 256 or raw['actual_logical_bytes'] != (2 * leaves - 1) * 32:
        raise ValueError(f'{path}: payload/capacity mismatch')
    if raw['warmups'] != warmups or raw['measured_trials'] != trials:
        raise ValueError(f'{path}: trial/warmup mismatch')
    if int(raw['metadata']['min_noise_budget_bits']) < 1:
        raise ValueError(f'{path}: noise margin failed')


def compare_results(baseline, candidate):
    for key in ('ring_degree', 'g', 'N0', 'b', 'key_height', 'response_modulus', 'ciphertext_modulus'):
        if baseline['metadata'][key] != candidate['metadata'][key]:
            raise ValueError(f'comparison changed {key}')
    if baseline['actual_logical_bytes'] != candidate['actual_logical_bytes']:
        raise ValueError('comparison changed database size')
    a, b = baseline['samples'], candidate['samples']
    if [(s['trial'], s['leaf']) for s in a] != [(s['trial'], s['leaf']) for s in b]:
        raise ValueError('comparison changed query schedule')
    ta, tb = fmean(s['server_total_ms'] for s in a), fmean(s['server_total_ms'] for s in b)
    stages = []
    if a[0]['stages_ms'].keys() != b[0]['stages_ms'].keys():
        raise ValueError('comparison changed stage definitions')
    for key in a[0]['stages_ms']:
        sa = fmean(s['stages_ms'][key] for s in a)
        sb = fmean(s['stages_ms'][key] for s in b)
        stages.append({'stage': key, 'baseline_ms': sa, 'candidate_ms': sb, 'saved_ms': sa-sb})
    mib = baseline['actual_logical_bytes'] / 2**20
    return {'baseline_ms': ta, 'candidate_ms': tb, 'speedup': ta/tb,
            'baseline_mib_s': mib/(ta/1000), 'candidate_mib_s': mib/(tb/1000),
            'stage_rows': stages,
            'baseline_communication': baseline['communication'],
            'candidate_communication': candidate['communication']}


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--baseline-binary', type=Path, required=True)
    parser.add_argument('--candidate-binary', type=Path, required=True)
    parser.add_argument('--leaf-count', type=int, required=True)
    parser.add_argument('--experiments', type=int, default=100)
    parser.add_argument('--warmup', type=int, default=3)
    parser.add_argument('--output-dir', type=Path, required=True)
    args = parser.parse_args()
    out = args.output_dir.resolve()
    out.mkdir(parents=True, exist_ok=True)
    manifest = {'state': 'running', 'leaf_count': args.leaf_count,
                'trials': args.experiments, 'warmups': args.warmup, 'runs': []}
    def save():
        (out/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
    save()
    results = []
    try:
        for t, binary in ((4099, args.baseline_binary), (257, args.candidate_binary)):
            raw_path = out/f't{t}.json'
            log = out/f't{t}.log'
            if raw_path.exists() or log.exists():
                raise ValueError(f'existing run output: {raw_path}')
            env = os.environ.copy()
            env['BENCHMARK_OUTPUT_JSON'] = str(raw_path)
            command = [str(binary.resolve()), '--test', 'tree_bench_g32',
                       '--leaf-count', str(args.leaf_count), '--experiments', str(args.experiments),
                       '--warmup', str(args.warmup), '--trial-seed', '7']
            run = {'t': t, 'binary': str(binary.resolve()), 'command': command, 'state': 'running'}
            manifest['runs'].append(run)
            save()
            process = run_measured(command, log_path=log, env=env)
            (out/f't{t}-process.json').write_text(json.dumps(process, indent=2)+'\n')
            if process['exit_code'] != 0:
                raise ValueError(f't={t} failed; see {log}')
            raw = json.loads(raw_path.read_text())
            validate_result(raw, raw_path, t, args.leaf_count, args.experiments, args.warmup)
            results.append(raw)
            run['state'] = 'complete'
            save()
            print(f't={t}: {args.experiments} correct measurements, noise margin passed', flush=True)
        comparison = compare_results(*results)
        (out/'comparison.json').write_text(json.dumps(comparison, indent=2)+'\n')
        rows = ['# Tree g32 plaintext-modulus comparison', '',
                f"Full-tree size: {(2*args.leaf_count-1)*32/2**20:.6f} MiB; 32B entries; {args.experiments} measurements and {args.warmup} warmups.", '',
                '| Scheme | Mean server ms | MiB/s |', '|---|---:|---:|',
                f"| t4099 / c12 | {comparison['baseline_ms']:.3f} | {comparison['baseline_mib_s']:.3f} |",
                f"| t257 / c8 | {comparison['candidate_ms']:.3f} | {comparison['candidate_mib_s']:.3f} |", '',
                '| Stage | t4099 ms | t257 ms | Saved ms |', '|---|---:|---:|---:|']
        rows += [f"| {s['stage']} | {s['baseline_ms']:.3f} | {s['candidate_ms']:.3f} | {s['saved_ms']:.3f} |" for s in comparison['stage_rows']]
        rows += ['', '| Scheme | Query modeled B | Response actual B | Keys modeled B | Min noise bits |',
                 '|---|---:|---:|---:|---:|']
        for raw in results:
            wire = raw['communication']
            rows.append(f"| t={raw['metadata']['plaintext_modulus']} | {wire['modeled_query_bytes_per_trial']} | {wire['actual_response_bytes_per_trial']} | {wire['modeled_helper_key_bytes']} | {raw['metadata']['min_noise_budget_bits']} |")
        rows += ['', 'Query and helper-key bytes are modeled; response bytes are serialized.',
                 'Both runs use the same query leaf schedule and public layout. Runs are sequential, not interleaved.',
                 'Noise and full-response correctness checks are outside all timed stages.', '']
        (out/'REPORT.md').write_text('\n'.join(rows))
        manifest['state'] = 'complete'
        save()
    except Exception as error:
        manifest['state'] = 'failed'
        manifest['error'] = str(error)
        if manifest['runs'] and manifest['runs'][-1]['state'] == 'running':
            manifest['runs'][-1]['state'] = 'failed'
        save()
        raise


if __name__ == '__main__':
    main()
