use std::{
    collections::BTreeMap,
    fs,
    io::Write,
    path::{Path, PathBuf},
};

use anyhow::{Context, Result, bail};
use poulpy_pir::{
    config::{DEFAULT_BASE2K, DEFAULT_K, DEFAULT_N},
    payload::Payload,
};
use serde::Serialize;
use serde_json::{Value, json};

use crate::{
    backend::{P, backend_name},
    cli::Args,
    database::LevelReport,
    execution::IterationReport,
    util::{Summary, mean_usize, summarize},
    workload::checked_pow2,
};

#[derive(Serialize)]
pub(crate) struct Report {
    schema_version: &'static str,
    scheme: &'static str,
    path_mode: &'static str,
    height: u32,
    leaf_count: u64,
    record_bytes: usize,
    logical_records: u64,
    logical_db_bytes: u64,
    backend_records: u64,
    batch_size: usize,
    warmups: usize,
    trials: usize,
    source_commit: String,
    wrapper_source_commit: String,
    parameters: Value,
    setup: Value,
    communication: Value,
    profiling_coverage: &'static str,
    warmup_samples: Vec<SampleReport>,
    samples: Vec<SampleReport>,
    model: &'static str,
    implementation_provenance: ImplementationProvenance,
    run_mode: &'static str,
    entry_size_bytes: usize,
    total_iterations: usize,
    root_included: bool,
    leaf_file: String,
    crypto_backend: &'static str,
    transport_mode: &'static str,
    metric_notes: MetricNotes,
    layout: LayoutReport,
    preprocessing_summary: Summary,
    measured_summary: MeasuredSummary,
    stage_definitions: BTreeMap<&'static str, &'static str>,
    levels: Vec<LevelReport>,
    results: Vec<IterationReport>,
}

#[derive(Serialize)]
struct ImplementationProvenance {
    implementation: &'static str,
    source_url: &'static str,
    source_checkout: String,
    official_author_code: bool,
    notes: &'static str,
}

#[derive(Serialize)]
struct MetricNotes {
    runtime_metrics: &'static str,
    communication_metrics: &'static str,
    preprocessing_metrics: &'static str,
    model_fields: &'static str,
}

#[derive(Serialize)]
struct LayoutReport {
    construction: &'static str,
    database_split: &'static str,
    per_level_databases: usize,
    column_height: usize,
    ring_degree_n: usize,
    payloads_per_column: usize,
    total_logical_tree_nodes: u64,
    total_padded_payload_capacity: u64,
    unused_padded_payload_slots: u64,
}

#[derive(Clone, Serialize)]
struct SampleReport {
    leaf: u64,
    server_ms: f64,
    query_ms: f64,
    decode_ms: f64,
    query_bytes: usize,
    response_bytes: usize,
    correct: bool,
    correct_entries: usize,
    path_entries: usize,
    stages_ms: BTreeMap<String, f64>,
}

#[derive(Serialize)]
struct MeasuredSummary {
    all_correct: bool,
    measured_count: usize,
    warmup_count: usize,
    query_ms: Summary,
    server_ms: Summary,
    decode_ms: Summary,
    wire_query_bytes_total_mean: f64,
    wire_response_bytes_total_mean: f64,
    noise_max_log2_max: f64,
    noise_std_log2_max: f64,
}

pub(crate) fn build_report(
    args: &Args,
    level_reports: Vec<LevelReport>,
    results: Vec<IterationReport>,
) -> Result<Report> {
    let leaf_count = checked_pow2(args.height)?;
    let total_logical_tree_nodes = checked_pow2(args.height + 1)? - 1;
    let total_padded_payload_capacity: u64 = level_reports
        .iter()
        .map(|level| level.payload_capacity as u64)
        .sum();
    let unused_padded_payload_slots = total_padded_payload_capacity - total_logical_tree_nodes;
    let source_commit = source_checkout()?;
    let wrapper_source_commit = wrapper_source_commit().unwrap_or_else(|_| "unknown".to_string());
    let samples_all: Vec<SampleReport> = results.iter().map(sample_report).collect();
    let warmup_samples = samples_all[..args.warmup].to_vec();
    let samples = samples_all[args.warmup..].to_vec();
    let column_height = DEFAULT_N;
    let payloads_per_column = column_height / P::EXPONENT;

    Ok(Report {
        schema_version: "path-baseline-v1",
        scheme: "inspire_layerwise",
        path_mode: "ancestor_root_included",
        height: args.height,
        leaf_count,
        record_bytes: 32,
        logical_records: total_logical_tree_nodes,
        logical_db_bytes: total_logical_tree_nodes * 32,
        backend_records: total_padded_payload_capacity,
        batch_size: 1,
        warmups: args.warmup,
        trials: args.trials,
        source_commit: source_commit.clone(),
        wrapper_source_commit,
        parameters: parameters(args.height, payloads_per_column),
        setup: setup_report(&level_reports),
        communication: json!({
            "query": "actual_serialized",
            "response": "actual_serialized",
            "query_label": "actual_poulpy_query_write_to",
            "response_label": "actual_poulpy_response_write_to"
        }),
        profiling_coverage: "exclusive_stages",
        warmup_samples,
        samples,
        model: "Layerwise-InsPIRe-full-path",
        implementation_provenance: ImplementationProvenance {
            implementation: "third_party_poulpy_pir",
            source_url: "https://github.com/poulpy-fhe/poulpy-pir",
            source_checkout: source_commit,
            official_author_code: false,
            notes: "Visible author publication/GitHub lookup did not surface official InsPIRe code; this run uses Poulpy's public InsPIRe implementation.",
        },
        run_mode: "real_crypto_decrypt",
        entry_size_bytes: 32,
        total_iterations: args.warmup + args.trials,
        root_included: true,
        leaf_file: args.leaf_file.display().to_string(),
        crypto_backend: backend_name(),
        transport_mode: "actual_poulpy_serialized_query_response",
        metric_notes: MetricNotes {
            runtime_metrics: "Actual local timings from Client::query, Server::respond_timed, and Client::decode, summed over all H+1 path levels.",
            communication_metrics: "Actual serialized Poulpy query and response byte lengths, summed over all H+1 path levels.",
            preprocessing_metrics: "Actual per-level Server::generate_query_mask and Server::offline timings, computed once and reused across all leaves in this run.",
            model_fields: "Logical tree-node counts and padded level capacities are model/accounting fields, not extra communication.",
        },
        layout: LayoutReport {
            construction: "InsPIRe interpolation",
            database_split: "one independent PIR database per tree level",
            per_level_databases: args.height as usize + 1,
            column_height,
            ring_degree_n: DEFAULT_N,
            payloads_per_column,
            total_logical_tree_nodes,
            total_padded_payload_capacity,
            unused_padded_payload_slots,
        },
        preprocessing_summary: summarize(level_reports.iter().map(|level| level.offline_ms)),
        measured_summary: measured_summary(&results),
        stage_definitions: stage_definitions(),
        levels: level_reports,
        results,
    })
}

pub(crate) fn write_json(path: &Path, report: &Report) -> Result<()> {
    if let Some(parent) = path.parent() {
        fs::create_dir_all(parent)
            .with_context(|| format!("create output directory {}", parent.display()))?;
    }
    let mut file =
        fs::File::create(path).with_context(|| format!("create output {}", path.display()))?;
    serde_json::to_writer_pretty(&mut file, report).context("write report json")?;
    file.write_all(b"\n").context("finish report json")?;
    Ok(())
}

fn parameters(height: u32, payloads_per_column: usize) -> Value {
    json!({
        "variant": "InsPIRe",
        "variant_family": "interpolation",
        "implementation": "poulpy-pir",
        "payload": "U256P65535",
        "backend": backend_name(),
        "default_n": DEFAULT_N,
        "default_base2k": DEFAULT_BASE2K,
        "default_k": DEFAULT_K,
        "database_split": "one independent PIR database per tree level",
        "root_included": true,
        "height": height,
        "per_level_requests_per_sample": height + 1,
        "ring_degree_n": DEFAULT_N,
        "column_height": DEFAULT_N,
        "payload_digits": P::EXPONENT,
        "payloads_per_column": payloads_per_column,
        "collapse": "Interpolation",
        "security_error_settings": {
            "client_secret_and_error_sampling": "Poulpy Client::query default OS entropy",
            "server_seed": "Poulpy ServerSeed::default public CRS",
            "noise_check": "Client::noise recorded per retrieved level node",
            "correctness_gate": "Client::decode output must equal shared synthetic_tree_node_bytes payload for every root-to-leaf entry"
        }
    })
}

fn measured_summary(results: &[IterationReport]) -> MeasuredSummary {
    let measured: Vec<&IterationReport> = results
        .iter()
        .filter(|result| result.phase == "measured")
        .collect();
    let warmup_count = results.len() - measured.len();
    let measured_count = measured.len();
    let all_correct = results.iter().all(|result| result.correct);
    let wire_query_bytes_total_mean =
        mean_usize(measured.iter().map(|result| result.wire_query_bytes));
    let wire_response_bytes_total_mean =
        mean_usize(measured.iter().map(|result| result.wire_response_bytes));
    let noise_max_log2_max = measured
        .iter()
        .map(|result| result.noise_max_log2_max)
        .fold(f64::NEG_INFINITY, f64::max);
    let noise_std_log2_max = measured
        .iter()
        .map(|result| result.noise_std_log2_max)
        .fold(f64::NEG_INFINITY, f64::max);
    MeasuredSummary {
        all_correct,
        measured_count,
        warmup_count,
        query_ms: summarize(measured.iter().map(|result| result.query_ms)),
        server_ms: summarize(measured.iter().map(|result| result.server_ms)),
        decode_ms: summarize(measured.iter().map(|result| result.decode_ms)),
        wire_query_bytes_total_mean,
        wire_response_bytes_total_mean,
        noise_max_log2_max,
        noise_std_log2_max,
    }
}

fn sample_report(result: &IterationReport) -> SampleReport {
    SampleReport {
        leaf: result.leaf_id,
        server_ms: result.server_ms,
        query_ms: result.query_ms,
        decode_ms: result.decode_ms,
        query_bytes: result.wire_query_bytes,
        response_bytes: result.wire_response_bytes,
        correct: result.correct,
        correct_entries: result.path_nodes.iter().filter(|node| node.correct).count(),
        path_entries: result.path_nodes.len(),
        stages_ms: result.server_phases_ms.clone(),
    }
}

fn setup_report(levels: &[LevelReport]) -> Value {
    let preprocess_ms: f64 = levels.iter().map(|level| level.offline_ms).sum();
    let query_mask_ms: f64 = levels.iter().map(|level| level.setup_query_mask_ms).sum();
    let mut offline_phases_ms = BTreeMap::new();
    for level in levels {
        for (name, value) in &level.offline_phases_ms {
            *offline_phases_ms.entry(name.clone()).or_insert(0.0) += value;
        }
    }
    json!({
        "preprocess_ms": preprocess_ms,
        "query_mask_ms": query_mask_ms,
        "helper_bytes": null,
        "per_level_databases": levels.len(),
        "offline_phases_ms": offline_phases_ms,
        "memoized_public_db_preprocessing": true
    })
}

fn stage_definitions() -> BTreeMap<&'static str, &'static str> {
    BTreeMap::from([
        (
            "setup_query_mask_ms",
            "Server materializes public CRS query masks once per level.",
        ),
        (
            "offline_ms",
            "Server-side query-independent preprocessing over the public database, once per level.",
        ),
        (
            "query_ms",
            "Client builds one fresh encrypted query for one level node.",
        ),
        (
            "server_ms",
            "Server answers one encrypted query for one level node.",
        ),
        (
            "decode_ms",
            "Client decrypts and decodes one 32-byte level node.",
        ),
        (
            "wire_query_bytes",
            "Actual Poulpy query serialization length.",
        ),
        (
            "wire_response_bytes",
            "Actual Poulpy response serialization length.",
        ),
    ])
}

fn source_checkout() -> Result<String> {
    let manifest = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    let repo = manifest.join("../../../.benchmarks/inspire-path/poulpy-pir");
    let output = std::process::Command::new("git")
        .args(["-C", repo.to_str().unwrap(), "rev-parse", "HEAD"])
        .output()
        .context("read Poulpy checkout revision")?;
    if !output.status.success() {
        bail!("git rev-parse failed for {}", repo.display());
    }
    Ok(String::from_utf8_lossy(&output.stdout).trim().to_string())
}

fn wrapper_source_commit() -> Result<String> {
    let manifest = PathBuf::from(env!("CARGO_MANIFEST_DIR"));
    let repo = manifest.join("../../..");
    let output = std::process::Command::new("git")
        .args(["-C", repo.to_str().unwrap(), "rev-parse", "HEAD"])
        .output()
        .context("read wrapper checkout revision")?;
    if !output.status.success() {
        bail!("git rev-parse failed for {}", repo.display());
    }
    Ok(String::from_utf8_lossy(&output.stdout).trim().to_string())
}
