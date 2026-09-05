#!/usr/bin/env python3
"""Run one benchmark process and report its own wait4 peak RSS in bytes."""
import argparse
import json
import os
import pathlib
import subprocess
import sys
import time


def cgroup_peak():
    if sys.platform != 'linux':
        return None
    try:
        for line in pathlib.Path('/proc/self/cgroup').read_text().splitlines():
            if line.startswith('0::'):
                group = line[3:].lstrip('/')
                value = (pathlib.Path('/sys/fs/cgroup') / group / 'memory.peak').read_text().strip()
                return int(value)
    except (OSError, ValueError):
        return None
    return None


def run_measured(command, *, log_path, env=None, cwd=None):
    start = time.monotonic()
    with pathlib.Path(log_path).open('w') as output:
        process = subprocess.Popen(command, stdout=output, stderr=subprocess.STDOUT,
                                   env=env, cwd=cwd)
        _, status, usage = os.wait4(process.pid, 0)
        process.returncode = os.waitstatus_to_exitcode(status)
    multiplier = 1 if sys.platform == 'darwin' else 1024
    return {
        'exit_code': process.returncode,
        'elapsed_seconds': time.monotonic() - start,
        'process_peak_rss_bytes': int(usage.ru_maxrss * multiplier),
        'ru_maxrss_unit': 'bytes' if sys.platform == 'darwin' else 'KiB',
        'peak_scope': 'single benchmark child process; includes setup, calibration and all candidates',
        'cgroup_memory_peak_bytes': cgroup_peak(),
        'cgroup_peak_scope': 'shared cgroup cumulative peak, when available',
    }


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--log', required=True, type=pathlib.Path)
    parser.add_argument('--output', required=True, type=pathlib.Path)
    parser.add_argument('command', nargs=argparse.REMAINDER)
    args = parser.parse_args()
    command = args.command[1:] if args.command[:1] == ['--'] else args.command
    if not command:
        parser.error('a benchmark command is required after --')
    result = run_measured(command, log_path=args.log)
    args.output.write_text(json.dumps(result, indent=2) + '\n')
    return result['exit_code'] if result['exit_code'] >= 0 else 128 - result['exit_code']


if __name__ == '__main__':
    sys.exit(main())
