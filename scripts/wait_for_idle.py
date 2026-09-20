#!/usr/bin/env python3
"""Wait for shared-host computation to become idle without signaling any job."""
import argparse
import json
import os
from pathlib import Path
import time

COMPUTE_NAMES = ('onion', 'python', 'rustc', 'cargo', 'cmake', 'cc1',
                 'g++', 'clang', 'seperated', 'pir_', 'benchmark')


def busy_jobs(root, proc_root=Path('/proc')):
    jobs = []
    for directory in proc_root.glob('[0-9]*'):
        try:
            if directory.stat().st_uid != os.getuid():
                continue
            state = (directory / 'stat').read_text().split(') ', 1)[1].split()[0]
            if state in ('T', 't', 'Z'):
                continue
            name = (directory / 'comm').read_text().strip()
            command = (directory / 'cmdline').read_bytes().replace(b'\0', b' ').decode(errors='replace')
            if str(root) in command:
                continue
            if any(token in name.lower() for token in COMPUTE_NAMES):
                jobs.append({'pid': int(directory.name), 'name': name, 'state': state})
        except (FileNotFoundError, ProcessLookupError, PermissionError):
            continue  # A process may finish during the snapshot.
    return jobs


def cpu_ticks():
    result = {}
    for line in Path('/proc/stat').read_text().splitlines():
        fields = line.split()
        if fields and fields[0].startswith('cpu') and fields[0] != 'cpu':
            result[fields[0]] = list(map(int, fields[1:9]))
    return result


def maximum_cpu_busy(before, after):
    activity = []
    for cpu, ticks in before.items():
        delta = [new - old for old, new in zip(ticks, after[cpu])]
        total = sum(delta)
        activity.append((total - delta[3] - delta[4]) / total if total else 0)
    return max(activity)


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--root', type=Path, required=True)
    root = parser.parse_args().root.resolve()
    metadata = root / 'metadata'
    metadata.mkdir(parents=True, exist_ok=True)
    quiet = 0
    while quiet < 2:
        jobs = busy_jobs(root)
        before = cpu_ticks()
        time.sleep(5)
        peak = maximum_cpu_busy(before, cpu_ticks())
        jobs += busy_jobs(root)
        quiet = quiet + 1 if not jobs and peak < 0.20 else 0
        (metadata / 'idle.json').write_text(json.dumps({
            'observed_at': time.time(), 'quiet_windows': quiet,
            'other_jobs': jobs, 'maximum_core_busy_fraction': peak,
        }, indent=2) + '\n')
        if quiet == 0:
            time.sleep(20)


if __name__ == '__main__':
    main()
