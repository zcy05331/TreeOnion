#!/usr/bin/env python3
"""Small encrypted h1/h3 checks before the 1 GiB entry-size matrix."""
import argparse
import json
import pathlib

from read_process_peak_memory import run_measured
from run_entry_size_sweep import DEFAULT_ENTRY_BYTES, entry_plan, env_for, validate_bundle_arities
from summarize_cloudlab_profiles import validate_profile

ROOT = pathlib.Path(__file__).resolve().parents[1]


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--binary', type=pathlib.Path, required=True)
    parser.add_argument('--output-dir', type=pathlib.Path, required=True)
    parser.add_argument('--source-commit')
    args = parser.parse_args()
    output = args.output_dir.resolve()
    output.mkdir(parents=True, exist_ok=True)
    checks = []
    for size in DEFAULT_ENTRY_BYTES:
        raw = output / f'e{size}b.json'
        env = env_for(entry_plan(size), raw, args.source_commit)
        env['SUPERRECORD_MEMORY_BYTES'] = str(2 * 1024 ** 3)
        env.pop('SUPERRECORD_PLAN_ONLY', None)
        command = [str(args.binary.resolve()), '--test', 'tree_superrecord_bench',
                   '--leaf-count', '4096', '--experiments', '2',
                   '--warmup', '1', '--trial-seed', '7']
        process = run_measured(command, log_path=output / f'e{size}b.log',
                               env=env, cwd=ROOT)
        (output / f'e{size}b-process.json').write_text(json.dumps(process, indent=2) + '\n')
        if process['exit_code'] != 0:
            raise RuntimeError(f'{size}B encrypted check failed; see {output}')
        data = json.loads(raw.read_text())
        validate_profile(data, raw, 2, 4096)
        validate_bundle_arities(data, raw)
        if data.get('entry_bits') != size * 8 or data.get('benchmark_bundle_arities') != [1, 3]:
            raise RuntimeError(f'{size}B entry/subset metadata mismatch')
        measured = [c for c in data['candidates'] if c.get('eligible') and c.get('samples')]
        checks.append({'entry_bytes': size, 'leaf_count': 4096,
                       'samples_per_candidate': 2, 'warmups': 1,
                       'measured_candidates': len(measured), 'passed': True})
        print(f'{size}B: encrypted h1/h3 check passed ({len(measured)} candidates)', flush=True)
    (output / 'validation.json').write_text(json.dumps({
        'scope': 'small encrypted smoke checks, not 1 GiB performance results',
        'source_commit': args.source_commit, 'checks': checks,
    }, indent=2) + '\n')
    return 0


if __name__ == '__main__':
    raise SystemExit(main())
