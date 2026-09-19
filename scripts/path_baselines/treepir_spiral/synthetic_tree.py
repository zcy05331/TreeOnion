from __future__ import annotations

from pathlib import Path


MASK64 = (1 << 64) - 1


def _splitmix64_once(value: int) -> int:
    value = (value + 0x9E3779B97F4A7C15) & MASK64
    value = ((value ^ (value >> 30)) * 0xBF58476D1CE4E5B9) & MASK64
    value = ((value ^ (value >> 27)) * 0x94D049BB133111EB) & MASK64
    return (value ^ (value >> 31)) & MASK64


def node_bytes(level: int, index: int) -> bytes:
    state = (
        0x747265654E6F6465
        ^ ((level & MASK64) * 0xD6E8FEB86659FD93)
        ^ ((index & MASK64) * 0xA5A3564E27F8862F)
    ) & MASK64
    out = bytearray()
    for word in range(4):
        state = _splitmix64_once((state + word) & MASK64)
        out.extend(state.to_bytes(8, "little"))
    return bytes(out)


def path_level_indices(height: int, leaf: int) -> list[tuple[int, int]]:
    if height < 1:
        raise ValueError("height must be positive")
    leaf_count = 1 << height
    if leaf < 0 or leaf >= leaf_count:
        raise ValueError(f"leaf {leaf} is outside [0, {leaf_count})")
    return [
        (level, leaf >> (height - level))
        for level in range(height + 1)
    ]


def path_indices(height: int, leaf: int) -> list[int]:
    return [(1 << level) - 1 + local for level, local in path_level_indices(height, leaf)]


def record_bytes(flat_bfs_id: int) -> bytes:
    if flat_bfs_id < 0:
        raise ValueError("flat BFS id must be non-negative")
    level = (flat_bfs_id + 1).bit_length() - 1
    local = flat_bfs_id - ((1 << level) - 1)
    return node_bytes(level, local)


def read_leaves(path: Path, *, height: int, warmup: int, trials: int) -> tuple[list[int], list[int]]:
    if warmup < 0 or trials < 0:
        raise ValueError("--warmup and --trials must be non-negative")
    if trials == 0:
        raise ValueError("--trials must be greater than zero")
    leaves: list[int] = []
    for line_no, line in enumerate(path.read_text(encoding="utf-8").splitlines(), 1):
        stripped = line.strip()
        if not stripped:
            continue
        try:
            leaf = int(stripped, 10)
        except ValueError as exc:
            raise ValueError(f"{path}:{line_no}: leaf is not an integer") from exc
        if leaf < 0 or leaf >= (1 << height):
            raise ValueError(f"{path}:{line_no}: leaf {leaf} is outside [0, {1 << height})")
        leaves.append(leaf)
    needed = warmup + trials
    if len(leaves) < needed:
        raise ValueError(f"{path}: contains {len(leaves)} leaves but {needed} are required")
    return leaves[:warmup], leaves[warmup:needed]
