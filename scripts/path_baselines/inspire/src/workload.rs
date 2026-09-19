use std::{fs, path::Path};

use anyhow::{Context, Result, anyhow};

pub(crate) fn read_leaf_file(path: &Path) -> Result<Vec<u64>> {
    let text =
        fs::read_to_string(path).with_context(|| format!("read leaf file {}", path.display()))?;
    let mut out = Vec::new();
    for token in text.split_whitespace().filter(|s| !s.is_empty()) {
        out.push(
            token
                .parse()
                .with_context(|| format!("parse leaf id token {token:?}"))?,
        );
    }
    Ok(out)
}

pub(crate) fn tree_node_payload(level: u32, node_index: u64) -> [u8; 32] {
    let mut state = 0x747265654e6f6465u64
        ^ (u64::from(level).wrapping_mul(0xd6e8feb86659fd93))
        ^ (node_index.wrapping_mul(0xa5a3564e27f8862f));
    let mut out = [0u8; 32];
    for word in 0..4u64 {
        state = splitmix64(state.wrapping_add(word));
        out[word as usize * 8..][..8].copy_from_slice(&state.to_le_bytes());
    }
    out
}

pub(crate) fn flat_bfs_id(level: u32, node_index: u64) -> u64 {
    ((1u64 << level) - 1) + node_index
}

pub(crate) fn checked_pow2(exp: u32) -> Result<u64> {
    1u64.checked_shl(exp)
        .ok_or_else(|| anyhow!("2^{exp} does not fit u64"))
}

fn splitmix64(mut x: u64) -> u64 {
    x = x.wrapping_add(0x9e3779b97f4a7c15);
    x = (x ^ (x >> 30)).wrapping_mul(0xbf58476d1ce4e5b9);
    x = (x ^ (x >> 27)).wrapping_mul(0x94d049bb133111eb);
    x ^ (x >> 31)
}
