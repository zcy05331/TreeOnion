from __future__ import annotations

from dataclasses import dataclass


COLORS = "ABCDEFGHIJKLMNOPQRSTUVWXYZ0123456789"


@dataclass(frozen=True)
class NumColor:
    color: str
    size: int


def _node_count(height: int) -> int:
    return (1 << (height + 1)) - 2


def color_index(color: str) -> int:
    index = COLORS.find(color)
    if index < 0:
        raise ValueError(f"unknown color {color!r}")
    return index


def balanced_color_sizes(height: int) -> list[int]:
    total = _node_count(height)
    base, larger = divmod(total, height)
    return [base + (1 if idx >= height - larger else 0) for idx in range(height)]


def _stable_sort(colors: list[NumColor]) -> list[NumColor]:
    return sorted(colors, key=lambda item: item.size)


def balanced_color_sequence(height: int) -> list[NumColor]:
    sizes = balanced_color_sizes(height)
    return _stable_sort([NumColor(COLORS[idx], sizes[idx]) for idx in range(height)])


def feasible_split(height: int, colors: list[NumColor]) -> tuple[list[NumColor], list[NumColor]]:
    a: list[NumColor] = []
    b: list[NumColor] = []
    if height == 2:
        if colors[0].size == 2:
            a.append(NumColor(colors[1].color, colors[1].size // 2))
            b.append(NumColor(colors[1].color, colors[1].size // 2))
        else:
            a.append(NumColor(colors[1].color, colors[1].size - 1))
            b.append(NumColor(colors[0].color, colors[0].size - 1))
    elif colors[0].size == 2:
        i = 1
        av = colors[i].size // 2
        bv = (colors[i].size + 1) // 2
        a.append(NumColor(colors[i].color, av))
        b.append(NumColor(colors[i].color, bv))
        sa, sb = av, bv
        i += 1
        while i < height:
            if sa < sb:
                av = (colors[i].size + 1) // 2
                bv = colors[i].size // 2
            else:
                av = colors[i].size // 2
                bv = (colors[i].size + 1) // 2
            a.append(NumColor(colors[i].color, av))
            b.append(NumColor(colors[i].color, bv))
            sa += av
            sb += bv
            i += 1
    else:
        i = 1
        av = colors[i].size - 1
        bv = colors[0].size - 1
        a.append(NumColor(colors[1].color, av))
        b.append(NumColor(colors[0].color, bv))
        sa, sb = av, bv
        i += 1
        av = (colors[i].size + colors[0].size - colors[i - 1].size + 1) // 2
        bv = colors[i - 1].size - colors[0].size + (colors[i].size + colors[0].size - colors[i - 1].size) // 2
        a.append(NumColor(colors[i].color, av))
        b.append(NumColor(colors[i].color, bv))
        sa += av
        sb += bv
        i += 1
        while i < height:
            if sa < sb:
                av = (colors[i].size + 1) // 2
                bv = colors[i].size // 2
            else:
                av = colors[i].size // 2
                bv = (colors[i].size + 1) // 2
            a.append(NumColor(colors[i].color, av))
            b.append(NumColor(colors[i].color, bv))
            sa += av
            sb += bv
            i += 1
    return _stable_sort(a), _stable_sort(b)


def path_node_ids(height: int, leaf: int) -> list[int]:
    leaf_count = 1 << height
    if leaf < 0 or leaf >= leaf_count:
        raise ValueError(f"leaf {leaf} is outside [0, {leaf_count})")
    node = leaf_count + leaf
    nodes = []
    for _ in range(height):
        nodes.append(node)
        node //= 2
    nodes.reverse()
    return nodes


def _path_positions(height: int, leaf: int) -> list[int]:
    ids = path_node_ids(height, leaf)
    positions = []
    for level in range(1, height + 1):
        first = 1 << level
        temp = (ids[level - 1] - first + 1) % 2
        positions.append(2 if temp == 0 else temp)
    return positions


def local_csa_indices(height: int, leaf: int) -> list[int]:
    return _map_leaf(height, leaf)[0]


def color_path_positions(height: int, leaf: int) -> list[int]:
    return _map_leaf(height, leaf)[1]


def _map_leaf(height: int, leaf: int) -> tuple[list[int], list[int]]:
    path = _path_positions(height, leaf)
    colors = balanced_color_sequence(height)
    count = [0] * height
    indices = [0] * height
    positions = [-1] * height
    seen = [False] * height
    for layer in range(height - 1):
        current_height = height - layer
        split_point = 2 if colors[0].size == 2 else 1
        if path[layer] <= split_point:
            root_color = colors[0].color
            count[color_index(colors[0].color)] += path[layer] - 1
            count[color_index(colors[1].color)] += 2 - split_point
            idx = color_index(colors[0].color)
            indices[idx] = count[idx]
            positions[idx] = layer + 1
            seen[idx] = True
        else:
            root_color = colors[1].color
            count[color_index(colors[1].color)] += path[layer] - split_point - 1
            count[color_index(colors[0].color)] += split_point
            idx = color_index(colors[1].color)
            indices[idx] = count[idx]
            positions[idx] = layer + 1
            seen[idx] = True

        a, b = feasible_split(current_height, colors)
        if path[layer] == 2:
            for item in a:
                if root_color != item.color:
                    count[color_index(item.color)] += item.size

        colors = a if path[layer] == 1 else b
        if len(colors) == 1:
            idx = color_index(colors[0].color)
            count[idx] += path[-1] - 1
            indices[idx] = count[idx]
            positions[idx] = height
            seen[idx] = True
        else:
            colors = _stable_sort(colors)

    missing = [COLORS[i] for i, ok in enumerate(seen) if not ok]
    if missing:
        raise ValueError(f"indexing did not produce colors {missing}")
    return indices, positions
