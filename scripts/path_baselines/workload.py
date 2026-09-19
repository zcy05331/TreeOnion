"""The existing Generic 32-byte database and leaf schedule, shared by adapters."""

MASK64 = (1 << 64) - 1
INCREMENT = 0x9E3779B97F4A7C15
RECORD_BYTES = 32
PATH_MODE = "ancestor_root_included"


def splitmix64_once(value):
    value = (value + INCREMENT) & MASK64
    value = ((value ^ (value >> 30)) * 0xBF58476D1CE4E5B9) & MASK64
    value = ((value ^ (value >> 27)) * 0x94D049BB133111EB) & MASK64
    return value ^ (value >> 31)


def node_bytes(level, index):
    """Byte-for-byte port of src/tree_database.cpp::synthetic_tree_node_bytes."""
    state = (0x747265654E6F6465 ^ ((level * 0xD6E8FEB86659FD93) & MASK64)
             ^ ((index * 0xA5A3564E27F8862F) & MASK64))
    result = bytearray()
    for word in range(4):
        state = splitmix64_once((state + word) & MASK64)
        result.extend(state.to_bytes(8, "little"))
    return bytes(result)


def record_bytes(flat_index):
    if flat_index < 0:
        raise ValueError("flat index must be nonnegative")
    level = (flat_index + 1).bit_length() - 1
    return node_bytes(level, flat_index - ((1 << level) - 1))


def path_indices(height, leaf):
    if not 1 <= height <= 30 or not 0 <= leaf < (1 << height):
        raise ValueError("height/leaf outside supported tree")
    return [(1 << level) - 1 + (leaf >> (height - level))
            for level in range(height + 1)]


def trial_leaves(leaf_count, warmups, trials, seed):
    """Floyd sampling used by make_benchmark_trial_plan; keep its output order."""
    count = warmups + trials
    if leaf_count < 2 or leaf_count & (leaf_count - 1) or warmups < 0 or trials < 1 or count > leaf_count:
        raise ValueError("invalid leaf count or trial counts")
    selected, seen = [], set()
    state = seed & MASK64
    for upper in range(leaf_count - count, leaf_count):
        candidate = splitmix64_once(state) % (upper + 1)
        state = (state + INCREMENT) & MASK64
        leaf = upper if candidate in seen else candidate
        selected.append(leaf)
        seen.add(leaf)
    return selected
