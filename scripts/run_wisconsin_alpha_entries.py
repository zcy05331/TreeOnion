#!/usr/bin/env python3
"""Finite, serial Wisconsin alpha/entry-size/t257 experiment matrix."""
import argparse
import datetime
import json
import os
from pathlib import Path
import subprocess
import sys

from run_entry_size_sweep import entry_plan, run_entry, mean_first_selection, load_json


def save_progress(root, progress):
    progress['updated_utc'] = datetime.datetime.now(datetime.timezone.utc).isoformat()
    path = root/'metadata/progress.json'
    temporary = path.with_suffix('.tmp')
    temporary.write_text(json.dumps(progress, indent=2)+'\n')
    temporary.replace(path)
    (root/'metadata/status').write_text(progress['status']+'\n')


def run_stage(root, progress, name, operation):
    progress['current_stage'] = name
    progress['stages'][name] = {'status':'running'}
    (root/'outputs'/name).mkdir(parents=True, exist_ok=True)
    save_progress(root, progress)
    try:
        result = operation(root/'outputs'/name)
        progress['stages'][name] = {'status':'complete', 'result':result}
    except Exception as error:
        progress['stages'][name] = {'status':'failed', 'error':str(error)}
    progress['finished_stages'].append(name)
    save_progress(root, progress)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--run-root', type=Path, required=True)
    args = parser.parse_args()
    root = args.run_root.resolve()
    source = root/'source'
    current = source/'build-native/Onion-PIR'
    baseline = root/'baseline-source/build-native/Onion-PIR'
    t257 = source/'build-t257/Onion-PIR'
    commit = (root/'metadata/source-commit.txt').read_text().strip()
    os.environ['SUPERRECORD_MEMORY_BYTES'] = str(80*2**30)
    progress = {'status':'running','source_commit':commit,
                'finished_stages':['preflight'],
                'stages':{'preflight':{'status':'complete'}},'current_stage':None}
    save_progress(root, progress)

    def command_stage(folder, command, env=None):
        with (folder/'driver.log').open('w') as log:
            subprocess.run(command, cwd=source, env=env, stdout=log,
                           stderr=subprocess.STDOUT, check=True)
        return {'command':command}

    def micro(folder):
        env = dict(os.environ, BENCHMARK_OUTPUT_JSON=str(folder/'profile.json'))
        result = command_stage(folder, [str(current),'--test','tree_superrecord_alpha',
                                        '--experiments','100','--warmup','3'], env)
        data = load_json(folder/'profile.json')
        if not data['all_equivalence_checks_passed'] or data['trials'] != 100:
            raise RuntimeError('alpha microbenchmark validation failed')
        return result
    run_stage(root, progress, 'alpha_micro', micro)

    run_stage(root, progress, 'alpha_ab', lambda folder: command_stage(folder, [
        sys.executable,'scripts/benchmark_alpha_ntt_ab.py',
        '--baseline',str(baseline),'--optimized',str(current),
        '--baseline-commit','5a05443 with benchmark-only shape pin',
        '--optimized-commit',commit,'--leaf-count','16777216',
        '--experiments','50','--memory-gib','80','--output',str(folder/'results')]))

    for size in (16,48,64,128):
        def entry(folder, size=size):
            plan = entry_plan(size)
            result = run_entry(folder, current, plan, source_commit=commit)
            return {'plan':plan,'selection':mean_first_selection(
                load_json(Path(result['raw_file'])), result['analysis'])}
        run_stage(root, progress, f'entry{size}', entry)

    run_stage(root, progress, 't257', lambda folder: command_stage(folder, [
        sys.executable,'scripts/run_tree_t257_comparison.py',
        '--baseline-binary',str(current),'--candidate-binary',str(t257),
        '--leaf-count','16777216','--experiments','100','--warmup','3',
        '--output-dir',str(folder/'results')]))
    progress['status'] = ('complete_with_failures'
                          if any(s['status']=='failed' for s in progress['stages'].values())
                          else 'complete')
    progress['current_stage'] = None
    save_progress(root, progress)
    return 0 if progress['status']=='complete' else 1


if __name__ == '__main__':
    raise SystemExit(main())
