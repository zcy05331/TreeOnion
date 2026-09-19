use std::{collections::BTreeMap, time::Instant};

use anyhow::{Context, Result, bail};
use serde::Serialize;

use crate::{
    database::LevelContext,
    util::ms,
    workload::{flat_bfs_id, tree_node_payload},
};

#[derive(Serialize)]
pub(crate) struct IterationReport {
    pub(crate) iteration: usize,
    pub(crate) phase: &'static str,
    pub(crate) leaf_id: u64,
    pub(crate) correct: bool,
    pub(crate) query_ms: f64,
    pub(crate) server_ms: f64,
    pub(crate) decode_ms: f64,
    pub(crate) wire_query_bytes: usize,
    pub(crate) wire_response_bytes: usize,
    pub(crate) noise_max_log2_max: f64,
    pub(crate) noise_std_log2_max: f64,
    pub(crate) path_nodes: Vec<PathNodeReport>,
    pub(crate) server_phases_ms: BTreeMap<String, f64>,
}

#[derive(Serialize)]
pub(crate) struct PathNodeReport {
    pub(crate) level: u32,
    pub(crate) node_index: u64,
    pub(crate) flat_bfs_id: u64,
    pub(crate) correct: bool,
    pub(crate) query_ms: f64,
    pub(crate) server_ms: f64,
    pub(crate) decode_ms: f64,
    pub(crate) wire_query_bytes: usize,
    pub(crate) wire_response_bytes: usize,
    pub(crate) noise_max_log2: f64,
    pub(crate) noise_std_log2: f64,
    pub(crate) server_phases_ms: BTreeMap<String, f64>,
}

struct NodeRun {
    report: PathNodeReport,
    ok: bool,
}

pub(crate) fn run_iteration(
    iteration: usize,
    phase: &'static str,
    height: u32,
    leaf_id: u64,
    levels: &mut [LevelContext],
) -> Result<IterationReport> {
    let mut path_nodes = Vec::with_capacity(levels.len());
    let mut correct = true;
    for level in 0..=height {
        let node_index = leaf_id >> (height - level);
        let run = run_node(&mut levels[level as usize], node_index)?;
        correct &= run.ok;
        path_nodes.push(run.report);
    }
    let query_ms = path_nodes.iter().map(|node| node.query_ms).sum();
    let server_ms = path_nodes.iter().map(|node| node.server_ms).sum();
    let decode_ms = path_nodes.iter().map(|node| node.decode_ms).sum();
    let wire_query_bytes = path_nodes.iter().map(|node| node.wire_query_bytes).sum();
    let wire_response_bytes = path_nodes.iter().map(|node| node.wire_response_bytes).sum();
    let noise_max_log2_max = path_nodes
        .iter()
        .map(|node| node.noise_max_log2)
        .fold(f64::NEG_INFINITY, f64::max);
    let noise_std_log2_max = path_nodes
        .iter()
        .map(|node| node.noise_std_log2)
        .fold(f64::NEG_INFINITY, f64::max);
    let mut server_phases_ms = BTreeMap::new();
    for node in &path_nodes {
        for (name, value) in &node.server_phases_ms {
            *server_phases_ms.entry(name.clone()).or_insert(0.0) += value;
        }
    }
    Ok(IterationReport {
        iteration,
        phase,
        leaf_id,
        correct,
        query_ms,
        server_ms,
        decode_ms,
        wire_query_bytes,
        wire_response_bytes,
        noise_max_log2_max,
        noise_std_log2_max,
        path_nodes,
        server_phases_ms,
    })
}

fn run_node(level: &mut LevelContext, node_index: u64) -> Result<NodeRun> {
    if node_index as usize >= level.logical_nodes {
        bail!(
            "node index {node_index} is out of range for level {}",
            level.level
        );
    }
    let payload_index = node_index as usize;
    let query_start = Instant::now();
    let (query, state) = level.client.query(payload_index);
    let query_ms = ms(query_start.elapsed());

    let mut qbuf = Vec::new();
    query
        .write_to(level.server.params().module(), &mut qbuf)
        .context("serialize query")?;

    let server_start = Instant::now();
    let (response, online) = level.server.respond_timed(&query);
    let server_ms = ms(server_start.elapsed());
    let server_phases_ms = online
        .phases()
        .iter()
        .map(|phase| (phase.name().to_string(), ms(phase.duration())))
        .collect();

    let mut rbuf = Vec::new();
    response
        .write_to(level.server.params().module(), &mut rbuf)
        .context("serialize response")?;

    let decode_start = Instant::now();
    let got = level.client.decode(&response, &state);
    let decode_ms = ms(decode_start.elapsed());
    let expected = tree_node_payload(level.level, node_index);
    let flat_bfs_id = flat_bfs_id(level.level, node_index);
    if got != expected {
        bail!(
            "decode mismatch at level {} node {} flat_bfs_id {}",
            level.level,
            node_index,
            flat_bfs_id
        );
    }

    let selected = state.address();
    let expected_record = level
        .server
        .database()
        .record(selected.column, selected.matrix);
    let noise = level.client.noise(&response, &state, &expected_record);
    Ok(NodeRun {
        ok: true,
        report: PathNodeReport {
            level: level.level,
            node_index,
            flat_bfs_id,
            correct: true,
            query_ms,
            server_ms,
            decode_ms,
            wire_query_bytes: qbuf.len(),
            wire_response_bytes: rbuf.len(),
            noise_max_log2: noise.max_log2(),
            noise_std_log2: noise.std_log2(),
            server_phases_ms,
        },
    })
}
