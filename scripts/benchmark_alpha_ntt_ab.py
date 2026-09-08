#!/usr/bin/env python3
"""Compare fixed h3 layouts before/after NTT-domain alpha preparation."""
import argparse
import json
import os
from pathlib import Path
from statistics import fmean

from read_process_peak_memory import run_measured
from summarize_cloudlab_profiles import finite_ms, require_count, require_leaf_count


def validate_h3_result(data, path, trials, leaves):
    require_leaf_count(data, path, leaves)
    require_count(data.get('measured_trials'), trials, path, 'measured trials')
    if data.get('entry_bits') != 256 or data.get('warmup_count') != 3:
        raise ValueError('expected 32-byte entries and three warmups')
    cs = [c for c in data['candidates'] if c.get('eligible') and c.get('samples')]
    if len(cs) != 1:
        raise ValueError('expected one measured h3 candidate')
    c = cs[0]
    if c['shape']['bundle_arity'] != 3:
        raise ValueError('expected h3')
    require_count(len(c['samples']), trials, path, 'h3 samples')
    gate = c['target_noise_result']
    if gate['decrypts_correctly'] is not True or gate['observed_key_coverage_matches_tag'] is not True:
        raise ValueError('h3 decryption/key coverage failed')
    for sample in c['samples']:
        for key in ('server_total_ms','client_query_ms','client_decode_ms','response_serialize_ms'):
            finite_ms(sample[key], path, key)
        for key, value in sample['stages_ms'].items():
            finite_ms(value, path, key)
    return c


def main():
    p = argparse.ArgumentParser(description=__doc__)
    p.add_argument('--baseline', type=Path, required=True)
    p.add_argument('--optimized', type=Path, required=True)
    p.add_argument('--baseline-commit', required=True)
    p.add_argument('--optimized-commit', required=True)
    p.add_argument('--output', type=Path, required=True)
    p.add_argument('--leaf-count', type=int, default=1 << 22)
    p.add_argument('--experiments', type=int, default=100)
    args = p.parse_args()
    out = args.output.resolve()
    out.mkdir(parents=True, exist_ok=True)
    manifest = {'state': 'running', 'runs': [], 'leaf_count': args.leaf_count,
                'samples_per_run': args.experiments, 'warmups_per_run': 3}
    def save():
        (out/'manifest.json').write_text(json.dumps(manifest, indent=2)+'\n')
    if (out/'manifest.json').exists():
        raise ValueError('choose a fresh output directory')
    save()
    results = {}
    try:
        for round_id, order in enumerate([['old','new'], ['new','old']]):
            for variant in order:
                name = f'round{round_id}-{variant}'
                raw = out/(name+'.json')
                binary = args.baseline if variant == 'old' else args.optimized
                commit = args.baseline_commit if variant == 'old' else args.optimized_commit
                env = os.environ.copy()
                for key in ('SUPERRECORD_PLAN_ONLY', 'SUPERRECORD_CANDIDATE_INDEX'):
                    env.pop(key, None)
                env.update({
                    'SUPERRECORD_ENTRY_BITS':'256', 'SUPERRECORD_BUNDLE_ARITIES':'3',
                    'SUPERRECORD_MAX_ARITY':'3', 'SUPERRECORD_SKIP_LEGACY':'1',
                    'SUPERRECORD_MAX_FRONTIER':'0', 'SUPERRECORD_H1_FRONTIER':'0',
                    'SUPERRECORD_EXPANSION_HEIGHTS':'9', 'SUPERRECORD_ALPHA_ALIGNMENTS':'16',
                    'SUPERRECORD_MAX_RESPONSES':'1', 'SUPERRECORD_FIXED_KEY_HEIGHT':'11',
                    'SUPERRECORD_MEMORY_BYTES':str(4*2**30), 'SUPERRECORD_MIN_NOISE_MARGIN':'0',
                    'SUPERRECORD_PIN_SHAPE':f'3,64,1,352,{args.leaf_count.bit_length()-1-15}',
                    'SUPERRECORD_OUTPUT_JSON':str(raw), 'SUPERRECORD_COMMIT':commit,
                })
                command = [str(binary.resolve()), '--test', 'tree_superrecord_bench',
                           '--leaf-count', str(args.leaf_count), '--experiments', str(args.experiments),
                           '--warmup', '3', '--trial-seed', '7']
                row = {'name':name, 'state':'running', 'command':command, 'source_commit':commit,
                       'environment':{k:v for k,v in env.items() if k.startswith('SUPERRECORD_')}}
                manifest['runs'].append(row)
                save()
                process = run_measured(command, log_path=out/(name+'.log'), env=env)
                (out/(name+'-process.json')).write_text(json.dumps(process, indent=2)+'\n')
                if process['exit_code'] != 0:
                    raise RuntimeError(f'{name} failed; inspect preserved log')
                data = json.loads(raw.read_text())
                c = validate_h3_result(data, raw, args.experiments, args.leaf_count)
                expected = {'bundle_arity':3, 'record_coeffs':64, 'plane_count':1,
                            'N0':352, 'beta_bits':args.leaf_count.bit_length()-1-15}
                if any(c['shape'][k] != v for k,v in expected.items()):
                    raise ValueError(f'{name}: calibrated layout changed; do not compare as fixed layout')
                if results:
                    first = next(iter(results.values()))
                    for key in ('shape','scheme_fingerprint','level_plans','wire'):
                        if c[key] != first[key]:
                            raise ValueError(f'{name}: comparison changed {key}')
                    if [(s['block_id'],s['leaf']) for s in c['samples']] != [(s['block_id'],s['leaf']) for s in first['samples']]:
                        raise ValueError('query schedules differ')
                results[name] = c
                row['state'] = 'complete'
                row['mean_server_ms'] = fmean(s['server_total_ms'] for s in c['samples'])
                row['mean_prepare_ms'] = fmean(s['stages_ms']['prepare'] for s in c['samples'])
                save()
                print(json.dumps(row), flush=True)
        summary = {}
        for variant in ('old','new'):
            samples = [s for name,c in results.items() if name.endswith('-'+variant) for s in c['samples']]
            summary[variant] = {'samples':len(samples),
                                'server_ms':fmean(s['server_total_ms'] for s in samples),
                                'stages_ms':{k:fmean(s['stages_ms'][k] for s in samples) for k in samples[0]['stages_ms']}}
        summary['speedup'] = summary['old']['server_ms']/summary['new']['server_ms']
        summary['communication'] = next(iter(results.values()))['wire']
        (out/'comparison.json').write_text(json.dumps(summary, indent=2)+'\n')
        manifest['state'] = 'complete'
        save()
    except Exception as error:
        manifest['state'] = 'failed'
        manifest['error'] = str(error)
        save()
        raise


if __name__ == '__main__':
    main()
