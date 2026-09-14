#!/usr/bin/env python3
import argparse
import json
import re


COLORS = list("ABCDEFGHIJKLMNOPQRSTUVWXYZ") + list("0123456789")


def path_nodes(height, leaf):
    node = (1 << height) + leaf
    out = []
    for _ in range(height):
        out.append(node)
        node //= 2
    return list(reversed(out))


def path_positions(height, leaf):
    nodes = path_nodes(height, leaf)
    positions = []
    for level, node in enumerate(nodes, start=1):
        temp = (node - (1 << level) + 1) % 2
        positions.append(2 if temp == 0 else temp)
    return positions


def sort_colors(seq):
    return sorted(seq, key=lambda item: item[1])


def balanced_color_sequence(height):
    total = (1 << (height + 1)) - 2
    base = total // height
    rem = total % height
    return sort_colors([(COLORS[i], base + (1 if i >= height - rem else 0)) for i in range(height)])


def feasible_split(height, c):
    a = []
    b = []
    if height == 2:
        if c[0][1] == 2:
            a.append((c[1][0], c[1][1] // 2))
            b.append((c[1][0], c[1][1] // 2))
        else:
            a.append((c[1][0], c[1][1] - 1))
            b.append((c[0][0], c[0][1] - 1))
    elif c[0][1] == 2:
        i = 1
        av = c[i][1] // 2
        bv = (c[i][1] + 1) // 2
        a.append((c[i][0], av))
        b.append((c[i][0], bv))
        sa, sb = av, bv
        i += 1
        while i < height:
            if sa < sb:
                av = (c[i][1] + 1) // 2
                bv = c[i][1] // 2
            else:
                av = c[i][1] // 2
                bv = (c[i][1] + 1) // 2
            a.append((c[i][0], av))
            b.append((c[i][0], bv))
            sa += av
            sb += bv
            i += 1
    else:
        i = 1
        av = c[i][1] - 1
        bv = c[0][1] - 1
        a.append((c[1][0], av))
        b.append((c[0][0], bv))
        sa, sb = av, bv
        i += 1
        av = (c[i][1] + c[0][1] - c[i - 1][1] + 1) // 2
        bv = c[i - 1][1] - c[0][1] + (c[i][1] + c[0][1] - c[i - 1][1]) // 2
        a.append((c[i][0], av))
        b.append((c[i][0], bv))
        sa += av
        sb += bv
        i += 1
        while i < height:
            if sa < sb:
                av = (c[i][1] + 1) // 2
                bv = c[i][1] // 2
            else:
                av = c[i][1] // 2
                bv = (c[i][1] + 1) // 2
            a.append((c[i][0], av))
            b.append((c[i][0], bv))
            sa += av
            sb += bv
            i += 1
    return sort_colors(a), sort_colors(b)


def plan_records(height, leaf):
    positions = path_positions(height, leaf)
    nodes = path_nodes(height, leaf)
    c = balanced_color_sequence(height)
    count = {color: 0 for color in COLORS[:height]}
    records = {}
    for level in range(height - 1):
        current_height = height - level
        x = 2 if c[0][1] == 2 else 1
        if positions[level] <= x:
            root = c[0][0]
            count[c[0][0]] += positions[level] - 1
            count[c[1][0]] += 2 - x
        else:
            root = c[1][0]
            count[c[1][0]] += positions[level] - x - 1
            count[c[0][0]] += x
        records[root] = {"node_id": nodes[level], "index": count[root]}

        a, b = feasible_split(current_height, c)
        if positions[level] == 2:
            for color, size in a:
                if root != color:
                    count[color] += size
        c = a if positions[level] == 1 else b
        if len(c) == 1:
            color = c[0][0]
            count[color] += positions[-1] - 1
            records[color] = {"node_id": nodes[-1], "index": count[color]}
        else:
            c = sort_colors(c)
    return records


def parse_java(path):
    blocks = {}
    current = None
    row_re = re.compile(r"color(.+?)_\d+_2\.json; NodeID: (\d+); Index: (\d+)")
    with open(path, encoding="ascii") as f:
        for raw in f:
            line = raw.strip()
            if line.startswith("TX_index: "):
                current = int(line.split(": ", 1)[1])
                blocks[current] = {}
                continue
            m = row_re.match(line)
            if not m or current is None:
                continue
            color, node, index = m.group(1), int(m.group(2)), int(m.group(3))
            blocks[current][color] = {"node_id": node, "index": index}
    return blocks


def main():
    parser = argparse.ArgumentParser()
    parser.add_argument("--height", type=int, required=True)
    parser.add_argument("--leaves0", required=True)
    parser.add_argument("--java", required=True)
    parser.add_argument("--out", required=True)
    args = parser.parse_args()

    leaves = [int(line.strip()) for line in open(args.leaves0, encoding="ascii") if line.strip()]
    java = parse_java(args.java)
    mismatches = []
    checked = 0
    for leaf in leaves:
        tx = leaf + 1
        expected = plan_records(args.height, leaf)
        observed = java.get(tx)
        if observed is None:
            mismatches.append({"leaf0": leaf, "tx_index": tx, "error": "missing_java_block"})
            continue
        for color in COLORS[: args.height]:
            checked += 1
            if observed.get(color) != expected.get(color):
                mismatches.append({
                    "leaf0": leaf,
                    "tx_index": tx,
                    "color": color,
                    "expected_cpp_plan": expected.get(color),
                    "java_subcsa": observed.get(color),
                })
                if len(mismatches) >= 20:
                    break
        if len(mismatches) >= 20:
            break

    total = (1 << (args.height + 1)) - 2
    base = total // args.height
    rem = total % args.height
    color_sequence = [
        {"color": color, "color_id": COLORS.index(color), "size": size}
        for color, size in balanced_color_sequence(args.height)
    ]
    result = {
        "height": args.height,
        "leaf_count": len(leaves),
        "checked_triples": checked,
        "pass": not mismatches,
        "mismatch_count_reported": len(mismatches),
        "mismatches": mismatches,
        "color_sequence": color_sequence,
        "balanced_base_size": base,
        "balanced_larger_color_count": rem,
    }
    with open(args.out, "w", encoding="ascii") as f:
        json.dump(result, f, indent=2, sort_keys=True)
        f.write("\n")
    print(json.dumps({k: result[k] for k in ("height", "leaf_count", "checked_triples", "pass", "mismatch_count_reported")}))


if __name__ == "__main__":
    main()
