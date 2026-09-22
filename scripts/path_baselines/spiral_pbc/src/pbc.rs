//! Bit-exact port of the OnionPIR cuckoo batch baseline (src/cuckoo_batch.cpp,
//! src/merkle_baseline.cpp, make_benchmark_trial_plan and the cuckoo benchmark's
//! sibling set), so a Spiral backend sees the same buckets, bucket positions,
//! trial leaves and records as the OnionPIR cuckoo rows.

/// splitmix64 finaliser used by the cuckoo hashes and the Merkle node oracle.
pub fn splitmix64_once(mut v: u64) -> u64 {
    v = v.wrapping_add(0x9e3779b97f4a7c15);
    v = (v ^ (v >> 30)).wrapping_mul(0xbf58476d1ce4e5b9);
    v = (v ^ (v >> 27)).wrapping_mul(0x94d049bb133111eb);
    v ^ (v >> 31)
}

/// Stateful splitmix64 step of make_benchmark_trial_plan.
fn splitmix64_next(state: &mut u64) -> u64 {
    *state = state.wrapping_add(0x9e3779b97f4a7c15);
    let mut v = *state;
    v = (v ^ (v >> 30)).wrapping_mul(0xbf58476d1ce4e5b9);
    v = (v ^ (v >> 27)).wrapping_mul(0x94d049bb133111eb);
    v ^ (v >> 31)
}

/// Seed the cuckoo benchmark passes to make_cuckoo_batch_params.
pub const BENCHMARK_HASH_SEED: u64 = 0x63756b32;

#[derive(Clone, Debug)]
pub struct CuckooParams {
    pub item_count: usize,
    pub batch_size: usize,
    pub num_buckets: usize,
    pub num_hashes: usize,
    pub seed: u64,
}

/// make_cuckoo_batch_params followed by the benchmark's `++num_buckets`.
pub fn benchmark_cuckoo_params(item_count: usize, batch_size: usize) -> CuckooParams {
    assert!(item_count > 0 && batch_size > 0, "empty batch or database");
    assert!(item_count <= u32::MAX as usize, "ordinals must fit u32");
    let base = std::cmp::max((3 * batch_size + 1) / 2, batch_size + 1);
    CuckooParams { item_count, batch_size, num_buckets: base + 1, num_hashes: 3, seed: BENCHMARK_HASH_SEED }
}

pub fn bucket_of(ordinal: u64, hash_index: usize, p: &CuckooParams) -> usize {
    let mixed = p.seed
        ^ 0x9e3779b97f4a7c15u64.wrapping_mul(hash_index as u64 + 1)
        ^ ordinal.wrapping_mul(0xd6e8feb86659fd93);
    (splitmix64_once(mixed) % p.num_buckets as u64) as usize
}

/// Sorted, deduplicated member lists; an item's public position is its rank.
pub fn build_buckets(p: &CuckooParams) -> Vec<Vec<u32>> {
    let mut buckets: Vec<Vec<u32>> = vec![Vec::new(); p.num_buckets];
    let expected = p.item_count * p.num_hashes / p.num_buckets + 1;
    for b in buckets.iter_mut() {
        b.reserve(expected + expected / 16);
    }
    for ordinal in 0..p.item_count as u64 {
        for j in 0..p.num_hashes {
            buckets[bucket_of(ordinal, j, p)].push(ordinal as u32);
        }
    }
    for b in buckets.iter_mut() {
        b.sort_unstable();
        b.dedup();
        assert!(!b.is_empty(), "every bucket must hold at least one item at this scale");
    }
    buckets
}

pub fn position_in_bucket(bucket: &[u32], ordinal: u32) -> usize {
    bucket.binary_search(&ordinal).expect("ordinal does not hash into this bucket")
}

/// Deterministic random-walk cuckoo placement (cuckoo_place).
pub fn cuckoo_place(ordinals: &[u32], p: &CuckooParams) -> Vec<Option<u32>> {
    assert_eq!(ordinals.len(), p.batch_size, "placement expects exactly the batch size");
    assert!(ordinals.len() < p.num_buckets, "more items than buckets can never place");
    let mut assignment: Vec<Option<u32>> = vec![None; p.num_buckets];
    let mut walk_state = p.seed ^ 0x66616365;
    for &start in ordinals {
        let mut current = start;
        let mut placed = false;
        for _step in 0..10000 {
            let mut free_bucket = p.num_buckets;
            for j in 0..p.num_hashes {
                let b = bucket_of(current as u64, j, p);
                if assignment[b] == Some(current) {
                    panic!("batch ordinals must be distinct");
                }
                if assignment[b].is_none() {
                    free_bucket = b;
                    break;
                }
            }
            if free_bucket != p.num_buckets {
                assignment[free_bucket] = Some(current);
                placed = true;
                break;
            }
            walk_state = splitmix64_once(walk_state);
            let j = (walk_state % p.num_hashes as u64) as usize;
            let b = bucket_of(current as u64, j, p);
            let evicted = assignment[b].unwrap();
            assignment[b] = Some(current);
            current = evicted;
        }
        assert!(placed, "eviction walk failed; retry with another seed");
    }
    assignment
}

/// Root-excluded flat Merkle database: 2 (2^H - 1) nodes, levels 1..H.
pub fn item_count(tree_height: usize) -> usize {
    2 * ((1usize << tree_height) - 1)
}

pub fn flat_ordinal(level: usize, local: usize) -> usize {
    (1usize << level) - 2 + local
}

pub fn node_of_ordinal(ordinal: usize) -> (usize, usize) {
    let heap = ordinal + 2;
    let level = (usize::BITS - 1 - heap.leading_zeros()) as usize;
    (level, heap - (1usize << level))
}

pub fn sibling_local(leaf: usize, tree_height: usize, level: usize) -> usize {
    (leaf >> (tree_height - level)) ^ 1
}

/// The k = H sibling ordinals of one leaf, levels H..1.
pub fn sibling_ordinals(leaf: usize, tree_height: usize) -> Vec<u32> {
    (1..=tree_height)
        .rev()
        .map(|level| flat_ordinal(level, sibling_local(leaf, tree_height, level)) as u32)
        .collect()
}

/// synthetic_merkle_node: deterministic 32-byte oracle record.
pub fn merkle_node(level: usize, local: usize) -> [u8; 32] {
    let mut node = [0u8; 32];
    let mut state: u64 = 0x6d65726b6c655f32
        ^ (level as u64).wrapping_mul(0xd6e8feb86659fd93)
        ^ (local as u64).wrapping_mul(0xa5a3564e27f8862f);
    for word in 0..4u64 {
        state = splitmix64_once(state.wrapping_add(word));
        node[(word as usize) * 8..(word as usize) * 8 + 8].copy_from_slice(&state.to_le_bytes());
    }
    node
}

/// make_benchmark_trial_plan: Floyd sampling without replacement.
pub fn trial_plan(leaf_count: usize, warmups: usize, measured: usize, seed: u64) -> (Vec<usize>, Vec<usize>) {
    assert!(leaf_count > 0 && measured > 0);
    let query_count = warmups + measured;
    assert!(query_count <= leaf_count, "distinct query IDs required");
    let mut selected: Vec<usize> = Vec::with_capacity(query_count);
    let mut state = seed;
    for offset in 0..query_count {
        let upper = leaf_count - query_count + offset;
        let candidate = (splitmix64_next(&mut state) % (upper as u64 + 1)) as usize;
        selected.push(if selected.contains(&candidate) { upper } else { candidate });
    }
    let measured_leaves = selected.split_off(warmups);
    (selected, measured_leaves)
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn ordinal_round_trip() {
        for level in 1..=12 {
            for local in [0usize, 1, (1 << level) - 1] {
                assert_eq!(node_of_ordinal(flat_ordinal(level, local)), (level, local));
            }
        }
        assert_eq!(flat_ordinal(1, 0), 0);
        assert_eq!(item_count(22), (1 << 23) - 2);
    }

    #[test]
    fn trial_plan_matches_cpp_seed_7() {
        // Measured leaves recorded by the C++ cuckoo benchmark on Utah (H=22 and H=24).
        let (_, m22) = trial_plan(1 << 22, 3, 8, 7);
        assert_eq!(m22, vec![489912, 3430182, 325581, 307198, 4049938, 1889243, 641477, 3537643]);
        let (_, m24) = trial_plan(1 << 24, 3, 8, 7);
        assert_eq!(m24, vec![13290720, 9622024, 8226918, 14471806, 8955543, 13755917, 1848245, 11926251]);
    }

    #[test]
    fn bucket_sizing_matches_cpp() {
        assert_eq!(benchmark_cuckoo_params(item_count(22), 22).num_buckets, 34);
        assert_eq!(benchmark_cuckoo_params(item_count(24), 24).num_buckets, 37);
        assert_eq!(benchmark_cuckoo_params(item_count(26), 26).num_buckets, 40);
    }

    #[test]
    fn h22_bucket_member_counts_match_cpp() {
        // Per-bucket member counts written by the C++ cuckoo benchmark at H=22.
        let text = include_str!("../tests/fixtures/cpp_h22_bucket_member_counts.json");
        let expected: Vec<usize> = serde_json::from_str(text).unwrap();
        let p = benchmark_cuckoo_params(item_count(22), 22);
        let counts: Vec<usize> = build_buckets(&p).iter().map(|b| b.len()).collect();
        assert_eq!(counts, expected);
    }

    #[test]
    fn placement_puts_every_sibling_in_one_of_its_buckets() {
        let h = 16;
        let p = benchmark_cuckoo_params(item_count(h), h);
        let buckets = build_buckets(&p);
        let (w, m) = trial_plan(1 << h, 3, 20, 7);
        for leaf in w.into_iter().chain(m) {
            let ords = sibling_ordinals(leaf, h);
            let a = cuckoo_place(&ords, &p);
            let mut seen = 0;
            for (b, slot) in a.iter().enumerate() {
                if let Some(o) = slot {
                    assert!(ords.contains(o));
                    position_in_bucket(&buckets[b], *o);
                    seen += 1;
                }
            }
            assert_eq!(seen, h);
        }
    }
}
