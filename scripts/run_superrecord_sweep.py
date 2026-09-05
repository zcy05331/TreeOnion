#!/usr/bin/env python3
"""Run the generic tree-PIR sweep and preserve raw measurements and provenance."""
import argparse
import datetime
import json
import os
import pathlib
import platform
import re
import shlex
import subprocess
import sys

from read_process_peak_memory import run_measured

ROOT = pathlib.Path(__file__).resolve().parents[1]


def command_text(command):
    try:
        result = subprocess.run(command, cwd=ROOT, text=True, capture_output=True, check=True)
        return result.stdout.strip()
    except (OSError, subprocess.CalledProcessError):
        return None


def environment_metadata(binary):
    cache = binary.parent / 'CMakeCache.txt'
    fields = {}
    if cache.exists():
        for line in cache.read_text().splitlines():
            if '=' in line and not line.startswith(('#', '//')):
                key, value = line.split('=', 1)
                fields[key.split(':', 1)[0]] = value
    compiler_command = fields.get('CMAKE_CXX_COMPILER', '/usr/bin/clang++').split(';')
    compiler_args = fields.get('CMAKE_CXX_COMPILER_ARG1', '')
    if not compiler_args:
        for config in (binary.parent / 'CMakeFiles').glob('*/CMakeCXXCompiler.cmake'):
            match = re.search(r'set\(CMAKE_CXX_COMPILER_ARG1 \"([^\"]*)\"\)', config.read_text())
            if match:
                compiler_args = match.group(1)
                break
    compiler_command += shlex.split(compiler_args)
    flags = binary.parent / 'CMakeFiles/Onion-PIR.dir/flags.make'
    return {
        'repository_commit': command_text(['git', 'rev-parse', 'HEAD']),
        'branch': command_text(['git', 'branch', '--show-current']),
        'uncommitted_changes': command_text(['git', 'status', '--porcelain']),
        'machine': platform.machine(),
        'platform': platform.platform(),
        'cpu_model': command_text(['sysctl', '-n', 'machdep.cpu.brand_string'])
                     if sys.platform == 'darwin' else platform.processor(),
        'physical_memory_bytes': command_text(['sysctl', '-n', 'hw.memsize'])
                                 if sys.platform == 'darwin' else None,
        'cpu_frequency_hz': None,
        'cpu_governor': None,
        'numa_binding': None,
        'compiler': compiler_command,
        'compiler_version': command_text(compiler_command + ['--version']),
        'compiler_flags': flags.read_text() if flags.exists() else None,
        'active_config': fields.get('ACTIVE_CONFIG'),
        'build_type': fields.get('CMAKE_BUILD_TYPE'),
        'target_architectures': fields.get('CMAKE_OSX_ARCHITECTURES'),
        'hexl_package_directory': fields.get('HEXL_DIR'),
        'hexl_version': pathlib.Path(fields['HEXL_DIR']).name.removeprefix('hexl-')
                        if fields.get('HEXL_DIR') else None,
        'binary': str(binary),
        'start_utc': datetime.datetime.now(datetime.timezone.utc).isoformat(),
        'timing_environment': 'local desktop; background activity and thermal/frequency drift are not controlled',
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=pathlib.Path,
                        default=ROOT / 'build-x86_64-benchmark/Onion-PIR')
    parser.add_argument('--output-dir', type=pathlib.Path)
    parser.add_argument('--leaf-count', type=int, default=4194304)
    parser.add_argument('--entry-bits', type=int, default=256)
    parser.add_argument('--profiles', action='store_true',
                        help='also run the required entry-size profiles at 256 leaves (large entry: 32)')
    parser.add_argument('--profiles-only', action='store_true')
    parser.add_argument('--trials', type=int)
    parser.add_argument('--warmup', type=int)
    parser.add_argument('--smoke', action='store_true')
    parser.add_argument('--plan-only', action='store_true')
    parser.add_argument('--seed', type=int, default=7)
    parser.add_argument('--execution-seed', type=int, default=20260904)
    parser.add_argument('--bootstrap-seed', type=int, default=90304)
    parser.add_argument('--max-frontier', type=int, default=2)
    parser.add_argument('--max-arity', type=int, default=8)
    parser.add_argument('--max-responses', type=int, default=1)
    parser.add_argument('--memory-gib', type=float, default=10)
    parser.add_argument('--key-mode', choices=['fixed', 'candidate'], default='fixed')
    args = parser.parse_args()
    binary = args.binary.resolve()
    if not binary.is_file():
        parser.error('build the benchmark binary before running the sweep')
    trials = args.trials if args.trials is not None else (2 if args.smoke else 16)
    warmup = args.warmup if args.warmup is not None else (1 if args.smoke else 3)
    if trials <= 0 or warmup < 0 or args.memory_gib <= 0:
        parser.error('trials and memory must be positive; warmup must be nonnegative')
    stamp = datetime.datetime.now(datetime.timezone.utc).strftime('%Y%m%dT%H%M%SZ')
    output = (args.output_dir or ROOT / 'outputs/superrecord' / stamp).resolve()
    output.mkdir(parents=True, exist_ok=True)
    tasks = [] if args.profiles_only else [(args.entry_bits, args.leaf_count, args.max_responses)]
    if args.profiles or args.profiles_only:
        tasks += [(e, 256, 2) for e in [13, 128, 160, 192, 256, 384, 512, 768]]
        tasks.append((30000, 32, 16))
    metadata = environment_metadata(binary)
    (output / 'environment.json').write_text(json.dumps(metadata, indent=2) + '\n')
    manifest = {'environment': metadata, 'runs': [], 'state': 'running'}
    manifest_path = output / 'manifest.json'
    for entry_bits, leaves, responses in tasks:
        label = f'e{entry_bits}-n{leaves}'
        raw = output / f'{label}.json'
        if raw.exists():
            raise ValueError(f'refusing to overwrite an existing raw run: {raw}')
        env = os.environ.copy()
        env.update({
            'SUPERRECORD_COMMIT': metadata['repository_commit'] or 'unavailable',
            'SUPERRECORD_BOOTSTRAP_SEED': str(args.bootstrap_seed),
            'SUPERRECORD_ENTRY_BITS': str(entry_bits),
            'SUPERRECORD_OUTPUT_JSON': str(raw),
            'SUPERRECORD_MAX_FRONTIER': str(args.max_frontier),
            'SUPERRECORD_MAX_ARITY': str(args.max_arity),
            'SUPERRECORD_MAX_RESPONSES': str(responses),
            'SUPERRECORD_EXECUTION_SEED': str(args.execution_seed),
            'SUPERRECORD_KEY_MODE': args.key_mode,
            'SUPERRECORD_MEMORY_BYTES': str(int(args.memory_gib * 1024 ** 3)),
        })
        if args.plan_only:
            env['SUPERRECORD_PLAN_ONLY'] = '1'
        command = [str(binary), '--test', 'tree_superrecord_bench',
                   '--leaf-count', str(leaves), '--experiments', str(trials),
                   '--warmup', str(warmup), '--trial-seed', str(args.seed)]
        print(f'{label}: {trials} measured trials, {warmup} warmups; log {output / (label + ".log")}', flush=True)
        record = {'label': label, 'raw_file': str(raw), 'command': command,
                  'benchmark_environment': {k: v for k, v in env.items() if k.startswith('SUPERRECORD_')}}
        record['process'] = run_measured(command, log_path=output / f'{label}.log',
                                         env=env, cwd=ROOT)
        manifest['runs'].append(record)
        manifest_path.write_text(json.dumps(manifest, indent=2) + '\n')
        if record['process']['exit_code'] != 0 or not raw.exists():
            manifest['state'] = 'failed'
            manifest_path.write_text(json.dumps(manifest, indent=2) + '\n')
            print(f'{label}: failed; inspect preserved log', file=sys.stderr)
            return 1
        try:
            data = json.loads(raw.read_text())
        except (OSError, ValueError) as error:
            record['error'] = str(error)
            manifest['state'] = 'failed'
            manifest_path.write_text(json.dumps(manifest, indent=2) + '\n')
            return 1
        data['environment'] = metadata
        data['process'] = record['process']
        raw.write_text(json.dumps(data, indent=2) + '\n')
        if not args.plan_only:
            analysis = [sys.executable, str(ROOT / 'scripts/analyze_superrecord_sweep.py'),
                        str(raw), '--seed', str(args.bootstrap_seed)]
            result = subprocess.run(analysis, cwd=ROOT, check=False)
            record['analysis_exit_code'] = result.returncode
            if result.returncode:
                manifest['state'] = 'failed'
                manifest_path.write_text(json.dumps(manifest, indent=2) + '\n')
                return result.returncode
        print(f'{label}: saved {raw}', flush=True)
    manifest['state'] = 'complete'
    manifest_path.write_text(json.dumps(manifest, indent=2) + '\n')
    print(f'Artifacts: {output}', flush=True)
    return 0


if __name__ == '__main__':
    sys.exit(main())
