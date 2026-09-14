#!/usr/bin/env python3
"""Compare compiled TreePIR index-only output with the original Java SubCSA."""
import argparse
import json
import re
from pathlib import Path


def main():
    parser = argparse.ArgumentParser(description=__doc__)
    parser.add_argument('--cpp', type=Path, required=True)
    parser.add_argument('--java', type=Path, required=True)
    parser.add_argument('--output', type=Path, required=True)
    args = parser.parse_args()
    cpp = json.loads(args.cpp.read_text())
    original = {}
    current = None
    pattern = re.compile(r'color(.+?)_\d+_2\.json; NodeID: (\d+); Index: (\d+)')
    for line in args.java.read_text().splitlines():
        if line.startswith('TX_index: '):
            current = int(line.split(': ', 1)[1])
            original[current] = {}
        match = pattern.match(line.strip())
        if match:
            color, node, index = match.groups()
            original[current][color] = (int(node), int(index))
    colors = 'ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789'[:cpp['height']]
    results = cpp['results']
    assert len(results) == cpp['trials'] + cpp['warmups'], 'missing compiled index results'
    checked = 0
    for result in results:
        java = original[result['leaf_id'] + 1]
        assert set(java) == set(colors), 'Java color coverage mismatch'
        assert sorted(node for node, _ in java.values()) == sorted(result['path_nodes']), 'path node mismatch'
        for position, color in enumerate(colors):
            assert result['sub_indices'][position] == java[color][1], (
                result['leaf_id'], color, result['sub_indices'][position], java[color][1])
            checked += 1
    summary = {'pass': True, 'height': cpp['height'], 'leaves_checked': len(results),
               'color_indices_checked': checked, 'cpp_source': str(args.cpp),
               'original_java_source': str(args.java), 'comparison': 'compiled C++ JSON versus original Java output'}
    args.output.write_text(json.dumps(summary, indent=2) + '\n')
    print(json.dumps(summary))


if __name__ == '__main__':
    main()
