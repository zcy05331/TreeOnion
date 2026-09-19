use crate::cli::Args;
use crate::params::SOURCE_COMMIT;
use crate::runner::TrialResult;
use respire::pir::pir::PIR;

fn ns_to_ms(ns: u128) -> f64 {
    ns as f64 / 1_000_000.0
}

fn json_usize_array(out: &mut String, values: &[usize]) {
    out.push('[');
    for (i, value) in values.iter().enumerate() {
        if i > 0 {
            out.push(',');
        }
        out.push_str(&value.to_string());
    }
    out.push(']');
}

fn sample_json(
    out: &mut String,
    result: &TrialResult,
    path_entries: usize,
    query_bytes: usize,
    response_bytes: usize,
) {
    out.push('{');
    out.push_str(&format!(
        "\"leaf\":{},\"server_ms\":{:.6},\"query_ms\":{:.6},\"decode_ms\":{:.6},",
        result.leaf,
        ns_to_ms(result.server_ns),
        ns_to_ms(result.query_ns),
        ns_to_ms(result.decode_ns)
    ));
    out.push_str(&format!(
        "\"query_bytes\":{},\"response_bytes\":{},\"correct\":true,\"correct_entries\":{},\"path_entries\":{},\"verified_dummy_entries\":{},\"total_verified_entries\":{},",
        query_bytes,
        response_bytes,
        result.correct_entries,
        path_entries,
        result.verified_dummy_entries,
        result.total_verified_entries
    ));
    out.push_str("\"path_indices\":");
    json_usize_array(out, &result.path_nodes);
    out.push_str(",\"request_indices\":");
    json_usize_array(out, &result.request_indices);
    out.push_str(",\"stages_ms\":{");
    for (i, (name, ns)) in result.stages.iter().enumerate() {
        if i > 0 {
            out.push(',');
        }
        out.push_str(&format!("\"{}\":{:.6}", name, ns_to_ms(*ns)));
    }
    out.push_str("}}");
}

pub fn render<P: PIR>(
    args: &Args,
    setup_ns: u128,
    results: &[TrialResult],
    query_bytes: usize,
    response_bytes: usize,
) -> String {
    let logical_records = (1usize << (args.height + 1)) - 1;
    let mut out = String::new();
    out.push_str("{\n");
    out.push_str("  \"schema_version\":\"path-baseline-v1\",\n");
    out.push_str("  \"scheme\":\"respire\",\n");
    out.push_str("  \"path_mode\":\"ancestor_root_included\",\n");
    out.push_str(&format!("  \"height\":{},\n", args.height));
    out.push_str(&format!("  \"leaf_count\":{},\n", 1usize << args.height));
    out.push_str(&format!("  \"record_bytes\":{},\n", P::BYTES_PER_RECORD));
    out.push_str(&format!("  \"logical_records\":{},\n", logical_records));
    out.push_str(&format!(
        "  \"logical_db_bytes\":{},\n",
        logical_records * P::BYTES_PER_RECORD
    ));
    out.push_str(&format!("  \"backend_records\":{},\n", P::NUM_RECORDS));
    out.push_str(&format!("  \"batch_size\":{},\n", P::BATCH_SIZE));
    out.push_str(&format!("  \"warmups\":{},\n", args.warmup));
    out.push_str(&format!("  \"trials\":{},\n", args.trials));
    out.push_str(&format!("  \"source_commit\":\"{}\",\n", SOURCE_COMMIT));
    out.push_str("  \"parameters\":{");
    out.push_str("\"record_mapping\":\"full_bfs_zero_based\",");
    out.push_str("\"cuckoo_buckets\":49,\"cuckoo_hashes\":3,");
    out.push_str("\"factory\":\"batch_256(49,8,nu1,nu2) with D3=64\",");
    out.push_str(
        "\"formal_h24_nu1\":8,\"formal_h24_nu2\":8,\"smoke_h8_nu1\":1,\"smoke_h8_nu2\":1},\n",
    );
    out.push_str(&format!(
        "  \"setup\":{{\"preprocess_keygen_ms\":{:.6},\"helper_bytes\":0}},\n",
        ns_to_ms(setup_ns)
    ));
    out.push_str(
        "  \"communication\":{\"query\":\"model_respire_params_query_size\",\"response\":\"model_respire_params_response_size\"},\n",
    );
    out.push_str("  \"profiling_coverage\":\"exclusive_stages\",\n");
    out.push_str("  \"warmup_samples\":[");
    let mut first = true;
    for result in results.iter().filter(|r| r.warmup) {
        if !first {
            out.push(',');
        }
        first = false;
        sample_json(
            &mut out,
            result,
            args.height + 1,
            query_bytes,
            response_bytes,
        );
    }
    out.push_str("],\n");
    out.push_str("  \"samples\":[");
    let mut first = true;
    for result in results.iter().filter(|r| !r.warmup) {
        if !first {
            out.push(',');
        }
        first = false;
        sample_json(
            &mut out,
            result,
            args.height + 1,
            query_bytes,
            response_bytes,
        );
    }
    out.push_str("]\n}\n");
    out
}
