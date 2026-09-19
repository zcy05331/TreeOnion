"""Finite sequential execution of explicitly selected external path adapters."""
import argparse
import json
import os
import sys
from pathlib import Path

from read_process_peak_memory import run_measured
from .report import summarize_result, validate_result
from .workload import PATH_MODE, trial_leaves

SCHEMES = ('spirit', 'spirit_rc', 'vbpir_pbc', 'inspire_layerwise',
           'treepir_spiral', 'respire')


def parse_binary(value):
    name, separator, path = value.partition('=')
    if not separator or name not in SCHEMES or not path:
        raise argparse.ArgumentTypeError('expected a supported scheme=adapter-path')
    return name, Path(path).resolve()


def adapter_command(scheme, binary, height, leaf_file, warmups, trials, output, extra_args=()):
    command = ([sys.executable, str(binary)] if binary.suffix == '.py' else [str(binary)])
    command += ['--height', str(height), '--leaf-file', str(leaf_file),
                '--warmup', str(warmups), '--trials', str(trials), '--output', str(output)]
    if scheme in ('spirit', 'spirit_rc'):
        command += ['--scheme', scheme]
    command.extend(extra_args)
    return command


def write_json(path, value):
    path.write_text(json.dumps(value, indent=2) + '\n')


def prepare_campaign(output, height, warmups, trials, seed):
    output.mkdir(parents=True, exist_ok=True)
    if (output / 'manifest.json').exists():
        raise ValueError('choose a fresh output directory; old runs are retained')
    leaves = trial_leaves(1 << height, warmups, trials, seed)
    leaf_file = output / 'leaves.txt'
    leaf_file.write_text(''.join(f'{leaf}\n' for leaf in leaves))
    manifest = {'state': 'running', 'path_mode': PATH_MODE, 'height': height,
                'entry_bytes': 32, 'trial_seed': seed, 'leaves': leaves,
                'warmups': warmups, 'trials': trials, 'runs': []}
    write_json(output / 'manifest.json', manifest)
    return manifest, leaf_file


def execute_adapter(scheme, binary, output, height, leaf_file, leaves, warmups, trials, extra_args=()):
    raw_path = output / f'{scheme}.json'
    command = adapter_command(scheme, binary, height, leaf_file, warmups, trials, raw_path, extra_args)
    record = {'scheme': scheme, 'state': 'running', 'command': command, 'raw': str(raw_path)}
    env = dict(os.environ)
    env.update({'OMP_NUM_THREADS': '1', 'OPENBLAS_NUM_THREADS': '1', 'MKL_NUM_THREADS': '1'})
    process = run_measured(command, log_path=output / f'{scheme}.log', env=env, cwd=output)
    write_json(output / f'{scheme}-process.json', process)
    record['process'] = process
    if process.get('exit_code') != 0:
        record.update(state='failed', error=f"adapter exited {process.get('exit_code')}")
        return record, None
    try:
        data = json.loads(raw_path.read_text())
        if data.get('scheme') != scheme:
            raise ValueError('adapter emitted a different scheme')
        validate_result(data, height=height, leaves=leaves, warmups=warmups, trials=trials)
        summary = summarize_result(data)
    except (ValueError, KeyError, OSError) as error:
        record.update(state='failed', error=str(error))
        return record, None
    summary['process_peak_rss_bytes'] = process.get('process_peak_rss_bytes')
    record['state'] = 'complete'
    return record, summary


def communication_cell(row, direction):
    measurement = row['communication'][direction]
    kind = 'actual' if measurement.startswith('actual_serialized') else 'model'
    return f"{row[direction + '_bytes']:.0f} ({kind})"


def write_summary(output, result):
    write_json(output / 'summary.json', result)
    lines = ['# Full-path baseline results', '',
             f"State: {result['state']}. All numbers below are validated measured samples.", '',
             '| Scheme | Samples | Server ms/path | Logical MiB/s | Query bytes | Response bytes |',
             '|---|---:|---:|---:|---:|---:|']
    for row in result['summaries']:
        lines.append(f"| {row['scheme']} | {row['completed_samples']} | {row['server_ms']:.3f} | "
                     f"{row['logical_mib_s']:.2f} | {communication_cell(row, 'query')} | "
                     f"{communication_cell(row, 'response')} |")
    lines += ['', 'Communication measurement types, setup costs, parameters and exclusive stage means are in summary.json.']
    for row in result['runs']:
        if row['state'] != 'complete':
            lines += ['', f"{row['scheme']}: {row.get('error', row['state'])}"]
    (output / 'RESULTS.md').write_text('\n'.join(lines) + '\n')
    stage_lines = ['# Native server stages (mean ms per complete path)', '']
    for row in result['summaries']:
        stage_lines += [f"## {row['scheme']}", '', '| Stage | Mean ms/path |', '|---|---:|']
        stage_lines += [f"| {stage} | {value:.6f} |" for stage, value in row['stages_ms'].items()]
        if not row['stages_ms']:
            stage_lines += ['Stage breakdown was not measured.']
        stage_lines += ['']
    (output / 'STAGES.md').write_text('\n'.join(stage_lines).rstrip() + '\n')


def run_campaign(binaries, output, *, height, warmups, trials, treepir_trials, seed, extra_args=None):
    output = output.resolve()
    if not 1 <= height <= 30 or not 0 < treepir_trials <= trials:
        raise ValueError('invalid height or TreePIR trial count')
    manifest, leaf_file = prepare_campaign(output, height, warmups, trials, seed)
    summaries = []
    for scheme, binary in binaries.items():
        count = treepir_trials if scheme == 'treepir_spiral' else trials
        manifest['current_scheme'] = scheme
        write_json(output / 'manifest.json', manifest)
        try:
            record, summary = execute_adapter(scheme, binary, output, height, leaf_file,
                                               manifest['leaves'], warmups, count, (extra_args or {}).get(scheme, []))
        except OSError as error:
            record, summary = {'scheme': scheme, 'state': 'failed', 'error': str(error)}, None
        manifest['runs'].append(record)
        if summary is not None:
            summaries.append(summary)
        write_json(output / 'manifest.json', manifest)
        print(f"{scheme}: {record['state']}", flush=True)
    manifest['state'] = ('complete' if all(x['state'] == 'complete' for x in manifest['runs'])
                         else 'complete_with_failures')
    manifest['current_scheme'] = None
    write_json(output / 'manifest.json', manifest)
    result = {'state': manifest['state'], 'runs': manifest['runs'], 'summaries': summaries}
    write_summary(output, result)
    return result


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', action='append', type=parse_binary, required=True)
    parser.add_argument('--adapter-arg', action='append', default=[], metavar='SCHEME=ARG')
    parser.add_argument('--height', type=int, default=24)
    parser.add_argument('--warmup', type=int, default=3)
    parser.add_argument('--trials', type=int, default=100)
    parser.add_argument('--treepir-trials', type=int, default=16)
    parser.add_argument('--seed', type=int, default=7)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    binaries = dict(args.binary)
    if len(binaries) != len(args.binary):
        parser.error('duplicate scheme names')
    for name, binary in binaries.items():
        if not binary.is_file():
            parser.error(f'{name}: adapter not found: {binary}')
    extra_args = {}
    for value in args.adapter_arg:
        name, separator, arg = value.partition('=')
        if not separator or name not in binaries or not arg:
            parser.error('--adapter-arg requires a selected SCHEME=ARG')
        extra_args.setdefault(name, []).append(arg)
    result = run_campaign(binaries, args.output, height=args.height, warmups=args.warmup,
                          trials=args.trials, treepir_trials=args.treepir_trials, seed=args.seed, extra_args=extra_args)
    return 0 if result['state'] == 'complete' else 1
