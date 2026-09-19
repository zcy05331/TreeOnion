use respire::pir::pir::{PIRRecordBytes, PIR};
use std::collections::HashSet;
use std::fs;
use std::path::PathBuf;

#[derive(Clone)]
pub struct TrialPlan {
    pub warmup: bool,
    pub leaf: usize,
    pub path_nodes: Vec<usize>,
    pub request_indices: Vec<usize>,
}

fn splitmix64_once(mut value: u64) -> u64 {
    value = value.wrapping_add(0x9e3779b97f4a7c15);
    value = (value ^ (value >> 30)).wrapping_mul(0xbf58476d1ce4e5b9);
    value = (value ^ (value >> 27)).wrapping_mul(0x94d049bb133111eb);
    value ^ (value >> 31)
}

pub fn node_bytes(level: usize, index: usize) -> [u8; 32] {
    let mut result = [0u8; 32];
    let mut state = 0x747265654e6f6465u64
        ^ (level as u64).wrapping_mul(0xd6e8feb86659fd93)
        ^ (index as u64).wrapping_mul(0xa5a3564e27f8862f);
    for word in 0..4usize {
        state = splitmix64_once(state.wrapping_add(word as u64));
        for byte in 0..8usize {
            result[word * 8 + byte] = (state >> (byte * 8)) as u8;
        }
    }
    result
}

fn level_and_local(flat_index: usize) -> (usize, usize) {
    let level = usize::BITS as usize - (flat_index + 1).leading_zeros() as usize - 1;
    (level, flat_index - ((1usize << level) - 1))
}

pub fn record_for<P: PIR>(height: usize, flat_index: usize) -> P::RecordBytes {
    let logical_records = (1usize << (height + 1)) - 1;
    if flat_index >= logical_records {
        return P::RecordBytes::default();
    }
    let (level, local) = level_and_local(flat_index);
    P::RecordBytes::from_bytes(&node_bytes(level, local)).unwrap()
}

pub fn path_indices(height: usize, leaf: usize) -> Vec<usize> {
    (0..=height)
        .map(|level| (1usize << level) - 1 + (leaf >> (height - level)))
        .collect()
}

pub fn read_leaves(path: &PathBuf, count: usize, height: usize) -> Result<Vec<usize>, String> {
    let text = fs::read_to_string(path)
        .map_err(|err| format!("cannot open leaf file {}: {err}", path.display()))?;
    let mut leaves = Vec::with_capacity(count);
    for (line_no, raw) in text.lines().enumerate() {
        let line = raw.split('#').next().unwrap_or("").trim();
        if line.is_empty() {
            continue;
        }
        let leaf = line
            .parse::<usize>()
            .map_err(|_| format!("{}:{} invalid leaf", path.display(), line_no + 1))?;
        if leaf >= (1usize << height) {
            return Err(format!(
                "{}:{} leaf {leaf} outside height {height}",
                path.display(),
                line_no + 1
            ));
        }
        leaves.push(leaf);
        if leaves.len() == count {
            break;
        }
    }
    if leaves.len() != count {
        return Err(format!(
            "leaf file has {} leaves; requested warmup+trials={count}",
            leaves.len()
        ));
    }
    Ok(leaves)
}

pub fn make_plans<P: PIR>(height: usize, leaves: &[usize], warmups: usize) -> Vec<TrialPlan> {
    leaves
        .iter()
        .enumerate()
        .map(|(i, &leaf)| {
            let path_nodes = path_indices(height, leaf);
            let mut used: HashSet<usize> = path_nodes.iter().copied().collect();
            let mut request_indices = path_nodes.clone();
            let mut dummy = P::NUM_RECORDS - 1;
            while request_indices.len() < P::BATCH_SIZE {
                if used.insert(dummy) {
                    request_indices.push(dummy);
                }
                dummy -= 1;
            }
            TrialPlan {
                warmup: i < warmups,
                leaf,
                path_nodes,
                request_indices,
            }
        })
        .collect()
}

#[cfg(test)]
mod tests {
    use super::*;
    use crate::params::SmokeH8;

    fn hex(bytes: &[u8]) -> String {
        bytes.iter().map(|b| format!("{b:02x}")).collect()
    }

    #[test]
    fn golden_bytes_match_shared_contract() {
        assert_eq!(
            hex(&node_bytes(0, 0)),
            "b8782335a8e76c4bf81ce235c978f777b654f924299739320bda3618dcc37790"
        );
        assert_eq!(
            hex(&node_bytes(3, 6)),
            "3849527f67cb7a2c02bb1678b692eb093d424f3e552d646e20c3159427402066"
        );
    }

    #[test]
    fn root_to_leaf_path_uses_full_bfs_indices() {
        assert_eq!(path_indices(3, 5), vec![0, 2, 5, 12]);
    }

    #[test]
    fn padding_keeps_path_prefix_and_distinct_batch() {
        let plans = make_plans::<SmokeH8>(8, &[192], 0);
        assert_eq!(plans[0].path_nodes.len(), 9);
        assert_eq!(plans[0].request_indices.len(), 32);
        assert_eq!(
            &plans[0].request_indices[..9],
            plans[0].path_nodes.as_slice()
        );
        assert_eq!(
            plans[0]
                .request_indices
                .iter()
                .copied()
                .collect::<HashSet<_>>()
                .len(),
            32
        );
        assert!(plans[0].request_indices[9..].contains(&(SmokeH8::NUM_RECORDS - 1)));
    }
}
