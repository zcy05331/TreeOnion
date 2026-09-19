use std::{collections::BTreeMap, time::Instant};

use anyhow::{Result, anyhow};
use poulpy_pir::{
    client::Client,
    config::{Collapse, Config},
    database::DatabaseLayout,
    server::Server,
};
use serde::Serialize;

use crate::{
    backend::{BE, P},
    util::ms,
    workload::{checked_pow2, tree_node_payload},
};

#[derive(Serialize)]
pub(crate) struct LevelReport {
    pub(crate) level: u32,
    pub(crate) logical_nodes: u64,
    pub(crate) rows: usize,
    pub(crate) cols: usize,
    pub(crate) payload_capacity: usize,
    pub(crate) padded_payloads: usize,
    pub(crate) setup_query_mask_ms: f64,
    pub(crate) offline_ms: f64,
    pub(crate) offline_phases_ms: BTreeMap<String, f64>,
}

pub(crate) struct LevelContext {
    pub(crate) level: u32,
    pub(crate) logical_nodes: usize,
    pub(crate) client: Client<BE, P>,
    pub(crate) server: Server<BE, P>,
    pub(crate) report: LevelReport,
}

pub(crate) fn build_level(
    level: u32,
    n: usize,
    column_height: usize,
    payloads_per_column: usize,
) -> Result<LevelContext> {
    let logical_nodes = usize::try_from(checked_pow2(level)?)
        .map_err(|_| anyhow!("level {level} node count does not fit usize"))?;
    let min_cols = logical_nodes.div_ceil(payloads_per_column).max(1);
    let cols = min_cols.div_ceil(n) * n;
    let rows = n;
    let layout = DatabaseLayout::<P>::new(rows, cols);
    let config = Config::<P>::with_collapse(Collapse::Interpolation);
    let mut server = Server::<BE, P>::new(config, layout);
    let client = Client::<BE, P>::new(config, layout);
    let capacity = layout.num_payloads(column_height);
    let mut values = Vec::with_capacity(capacity);
    for node in 0..capacity {
        let payload = if node < logical_nodes {
            tree_node_payload(level, node as u64)
        } else {
            [0u8; 32]
        };
        values.push(payload);
    }
    server.update_shard(0, &values);

    let setup_start = Instant::now();
    server.generate_query_mask();
    let setup_query_mask_ms = ms(setup_start.elapsed());
    let offline = server.offline();
    let offline_ms = ms(offline.total());
    let offline_phases_ms = offline
        .phases()
        .iter()
        .map(|phase| (phase.name().to_string(), ms(phase.duration())))
        .collect();

    Ok(LevelContext {
        level,
        logical_nodes,
        client,
        server,
        report: LevelReport {
            level,
            logical_nodes: logical_nodes as u64,
            rows,
            cols,
            payload_capacity: capacity,
            padded_payloads: capacity - logical_nodes,
            setup_query_mask_ms,
            offline_ms,
            offline_phases_ms,
        },
    })
}
