//! TreePIR's color buckets with Spiral (Blyss spiral-rs) as the per-color PIR.
//! The colors, color-local indices and leaves are the TreePIR row's: they are
//! read from its formal.json, whose rows carry the upstream SubCSA indices of
//! each leaf's H proof nodes (root excluded). Each color is a Spiral database
//! of TreePIR's balanced size filled with the SealPIR adapter's deterministic
//! records; every color shares one parameter set and one public-parameter
//! upload, as the buckets of spiral_pbc do, and is answered by the same
//! instrumented process_query. Colors are served one at a time (build, answer
//! every path's query for that color, release), so one color database is
//! resident at a time; a path's server time is the sum over its colors.

use serde_json::{json, Value};
use spiral_pbc::pbc::trial_plan;
use spiral_pbc::spiral::*;
use spiral_rs::aligned_memory::AlignedMemory64;
use spiral_rs::client::{Client, PublicParameters, Query};
use spiral_rs::params::Params;
use spiral_rs::server::{load_db_from_seek, process_query};
use spiral_rs::util::params_from_json_obj;
use std::io::Cursor;
use std::time::Instant;

struct Args {
    input: String,
    output: String,
    trial_seed: u64,
    nu1: usize,
    max_nu2: usize,
}

fn parse_args() -> Args {
    let mut a = Args { input: String::new(), output: String::new(), trial_seed: 7, nu1: 9, max_nu2: 6 };
    let argv: Vec<String> = std::env::args().collect();
    let mut i = 1;
    while i < argv.len() {
        let v = argv.get(i + 1).unwrap_or_else(|| panic!("{} needs a value", argv[i]));
        match argv[i].as_str() {
            "--input" => a.input = v.clone(),
            "--output" => a.output = v.clone(),
            "--trial-seed" => a.trial_seed = v.parse().unwrap(),
            "--nu1" => a.nu1 = v.parse().unwrap(),
            "--max-nu2" => a.max_nu2 = v.parse().unwrap(),
            other => panic!("unknown argument {other}"),
        }
        i += 2;
    }
    assert!(!a.input.is_empty() && !a.output.is_empty(), "--input (a TreePIR formal.json) and --output are required");
    a
}

/// splitmix64 as in scripts/treepir_sealpir/treepir_sealpir_adapter.cpp.
fn mix(mut x: u64) -> u64 {
    x = x.wrapping_add(0x9e3779b97f4a7c15);
    x = (x ^ (x >> 30)).wrapping_mul(0xbf58476d1ce4e5b9);
    x = (x ^ (x >> 27)).wrapping_mul(0x94d049bb133111eb);
    x ^ (x >> 31)
}

/// The SealPIR adapter's deterministic_entry: record `index` of `color`.
fn color_record(height: usize, color: usize, index: u64) -> [u8; RECORD_BYTES] {
    let mut state = mix(((height as u64) << 48) ^ ((color as u64) << 40) ^ index);
    let mut entry = [0u8; RECORD_BYTES];
    for (i, byte) in entry.iter_mut().enumerate() {
        if i % 8 == 0 {
            state = mix(state.wrapping_add(i as u64));
        }
        *byte = (state >> (8 * (i % 8))) as u8;
    }
    entry
}

/// TreePIR's H colors over the 2^(H+1) - 2 non-root nodes; the last
/// (total mod H) colors hold one more record (adapter_input.cpp).
fn balanced_color_sizes(height: usize) -> Vec<usize> {
    let total = (1usize << (height + 1)) - 2;
    let (base, larger) = (total / height, total % height);
    (0..height).map(|i| base + usize::from(i >= height - larger)).collect()
}

struct Row {
    phase: String,
    leaf: usize,
    sub_indices: Vec<usize>,
}

fn main() {
    let args = parse_args();
    let input: Value = serde_json::from_str(&std::fs::read_to_string(&args.input).unwrap()).unwrap();
    let h = input["height"].as_u64().unwrap() as usize;
    let warmups = input["warmups"].as_u64().unwrap() as usize;
    let trials = input["trials"].as_u64().unwrap() as usize;
    let rows: Vec<Row> = input["results"]
        .as_array()
        .unwrap()
        .iter()
        .map(|r| Row {
            phase: r["phase"].as_str().unwrap().to_string(),
            leaf: r["leaf_id"].as_u64().unwrap() as usize,
            sub_indices: r["sub_indices"].as_array().unwrap().iter().map(|x| x.as_u64().unwrap() as usize).collect(),
        })
        .collect();
    let leaf_count = 1usize << h;
    let sizes = balanced_color_sizes(h);
    let (warm, measured) = trial_plan(leaf_count, warmups, trials, args.trial_seed);
    let leaves: Vec<usize> = rows.iter().map(|r| r.leaf).collect();
    assert_eq!(leaves, [warm.clone(), measured.clone()].concat(), "input leaves are not the seed-{} schedule", args.trial_seed);
    for r in &rows {
        assert_eq!(r.sub_indices.len(), h, "one index per color");
        assert!(r.sub_indices.iter().zip(&sizes).all(|(&i, &s)| i < s), "a color index is outside its color");
    }

    let max_members = *sizes.iter().max().unwrap();
    let (nu2, instances) = spiral_shape(max_members, args.nu1, args.max_nu2);
    let params_json = spiral_params_json(args.nu1, nu2, instances);
    let params: Params = params_from_json_obj(&params_json);
    let per_item = params.db_item_size / RECORD_BYTES;
    let num_items = 1usize << (params.db_dim_1 + params.db_dim_2);
    assert!(num_items * per_item >= max_members);
    eprintln!("treepir_spiral: H={h} colors={h} max_color={max_members} nu1={} nu2={} instances={} items/color={num_items}",
              params.db_dim_1, params.db_dim_2, params.instances);

    // Client: one key set for every color (identical parameters).
    let setup_start = Instant::now();
    let mut client = Client::init(&params);
    let pp_client = client.generate_keys();
    let pp_bytes = pp_client.serialize();
    let pp_server = PublicParameters::deserialize(&params, &pp_bytes);
    let mut offline_setup_ms = setup_start.elapsed().as_secs_f64() * 1e3;

    // Per path: client, server and decode time, bytes and stages, summed over colors.
    let mut client_ms = vec![0f64; rows.len()];
    let mut server_ms = vec![0f64; rows.len()];
    let mut decode_ms = vec![0f64; rows.len()];
    let mut q_bytes = vec![0usize; rows.len()];
    let mut r_bytes = vec![0usize; rows.len()];
    let mut stages = vec![Stages::default(); rows.len()];
    let (mut db_bytes, mut checked_upstream) = (0usize, false);
    for (color, &size) in sizes.iter().enumerate() {
        // Server: this color's Spiral database, records in color-local order.
        let t = Instant::now();
        let mut bytes = vec![0u8; num_items * params.db_item_size];
        for index in 0..size {
            let off = (index / per_item) * params.db_item_size + (index % per_item) * RECORD_BYTES;
            bytes[off..off + RECORD_BYTES].copy_from_slice(&color_record(h, color, index as u64));
        }
        let db: AlignedMemory64 = load_db_from_seek(&params, &mut Cursor::new(bytes));
        db_bytes = db_bytes.max(db.len() * 8);
        offline_setup_ms += t.elapsed().as_secs_f64() * 1e3;

        for (row_index, row) in rows.iter().enumerate() {
            let index = row.sub_indices[color];
            let t = Instant::now();
            let query = client.generate_query(index / per_item);
            let wire = query.serialize();
            client_ms[row_index] += t.elapsed().as_secs_f64() * 1e3;
            q_bytes[row_index] += wire.len();
            let server_query = Query::deserialize(&params, &wire);

            let t = Instant::now();
            let (response, st) = process_query_timed(&params, &pp_server, &server_query, db.as_slice());
            server_ms[row_index] += t.elapsed().as_secs_f64() * 1e3;
            stages[row_index].add(&st);
            r_bytes[row_index] += response.len();
            if !checked_upstream {
                // Instrumented path must match upstream process_query byte for byte.
                let upstream = process_query(&params, &pp_server, &server_query, db.as_slice());
                assert!(upstream == response, "instrumented process_query diverges from upstream");
                checked_upstream = true;
            }

            let t = Instant::now();
            let item = client.decode_response(&response);
            decode_ms[row_index] += t.elapsed().as_secs_f64() * 1e3;
            let off = (index % per_item) * RECORD_BYTES;
            assert!(item[off..off + RECORD_BYTES] == color_record(h, color, index as u64),
                    "TreePIR+Spiral record mismatch at leaf {} color {color}", row.leaf);
        }
    }
    assert!(q_bytes.iter().all(|&b| b == q_bytes[0]) && r_bytes.iter().all(|&b| b == r_bytes[0]),
            "communication shape changed between paths");

    let mut samples: Vec<Value> = Vec::new();
    for (row_index, row) in rows.iter().enumerate() {
        if row.phase == "measured" {
            samples.push(json!({
                "trial": row_index - warmups, "leaf": row.leaf, "client_query_ms": client_ms[row_index],
                "server_total_ms": server_ms[row_index], "client_decode_ms": decode_ms[row_index],
                "query_bytes": q_bytes[row_index], "response_bytes": r_bytes[row_index],
                "stages_ms": stages[row_index].to_json(), "correct": true
            }));
        }
    }
    assert_eq!(samples.len(), trials);

    let (q_bytes, r_bytes) = (q_bytes[0], r_bytes[0]);
    let pick = |k: &str| samples.iter().map(|s| s[k].as_f64().unwrap()).collect::<Vec<f64>>();
    let stage_mean = |k: &str| mean(&samples.iter().map(|s| s["stages_ms"][k].as_f64().unwrap()).collect::<Vec<f64>>());
    let report = json!({
        "schema_version": "treepir_spiral_profile.v1",
        "scheme": "treepir_spiral",
        "upstream": {"repository": UPSTREAM_REPO, "commit": UPSTREAM_REV, "crate": "lib/spiral-rs"},
        "path_semantics": "legacy_colored_path_omits_root",
        "height": h, "leaf_count": leaf_count, "item_count": sizes.iter().sum::<usize>(), "record_bytes": RECORD_BYTES,
        "actual_logical_bytes": sizes.iter().sum::<usize>() * RECORD_BYTES,
        "warmups": warmups, "measured_trials": trials, "trial_seed": args.trial_seed,
        "warmup_leaves": warm, "measured_leaves": measured,
        "treepir": {
            "colors": h, "color_sizes": sizes, "max_color_records": max_members,
            "index_source": args.input, "index_model": input["model"], "index_upstream_commit": input["upstream_commit"],
            "records": "deterministic_entry(height, color, index) of scripts/treepir_sealpir, as in the TreePIR row"
        },
        "spiral_parameters": {
            "json": params_json, "records_per_item": per_item, "items_per_color": num_items,
            "source": "spiral-rs CFG_20_256 lattice parameters with nu_2 sized to the largest color",
            "max_nu2": args.max_nu2
        },
        "setup": {
            "offline_setup_ms": offline_setup_ms,
            "offline_setup_kind": "key generation plus every color's database encoding",
            "preprocessed_bytes_per_color": db_bytes,
            "padded_color_bytes": h * num_items * params.db_item_size,
            "resident": "one color database at a time"
        },
        "communication": {
            "public_parameter_bytes": pp_bytes.len(),
            "public_parameter_kind": "actual PublicParameters::serialize, uploaded once per session and shared by all colors",
            "query_bytes_per_path": q_bytes, "query_kind": "actual Query::serialize summed over all colors",
            "response_bytes_per_path": r_bytes, "response_kind": "actual process_query output summed over all colors"
        },
        "timing_boundaries": {
            "server_total_ms": "sum over colors of the instrumented process_query (expand, first_dim, fold, pack, encode), each color answered with its database resident",
            "client_query_ms": "generate_query + serialize, summed over colors",
            "client_decode_ms": "decode_response, summed over colors; record comparison is outside the timer",
            "threads": "rayon pool; run with RAYON_NUM_THREADS=1 and one pinned CPU"
        },
        "samples": samples,
        "measured_summary": {
            "count": trials,
            "server_ms_mean": mean(&pick("server_total_ms")),
            "client_query_ms_mean": mean(&pick("client_query_ms")),
            "client_decode_ms_mean": mean(&pick("client_decode_ms")),
            "stages_ms_mean": {"expand": stage_mean("expand"), "first_dim": stage_mean("first_dim"),
                               "fold": stage_mean("fold"), "pack": stage_mean("pack"), "encode": stage_mean("encode")},
            "all_correct": true
        }
    });
    std::fs::write(&args.output, serde_json::to_string_pretty(&report).unwrap() + "\n").unwrap();
    eprintln!("treepir_spiral: H={h} mean server {:.1} ms, query {} B, response {} B, public params {} B",
              mean(&pick("server_total_ms")), q_bytes, r_bytes, pp_bytes.len());
}

#[cfg(test)]
mod tests {
    use super::*;

    #[test]
    fn color_sizes_partition_the_non_root_nodes_like_the_sealpir_adapter() {
        // The H=22 row of the ICC TreePIR run: 381300 x 16 then 381301 x 6.
        let sizes = balanced_color_sizes(22);
        assert_eq!(sizes.iter().sum::<usize>(), (1 << 23) - 2);
        assert!(sizes[..16].iter().all(|&s| s == 381300) && sizes[16..].iter().all(|&s| s == 381301));
    }

    #[test]
    fn records_match_the_sealpir_adapter() {
        // deterministic_entry from treepir_sealpir_adapter.cpp, compiled and run.
        let hex = |e: [u8; RECORD_BYTES]| e.iter().map(|b| format!("{b:02x}")).collect::<String>();
        assert_eq!(hex(color_record(24, 3, 1398101)), "1d528cc29517a9770e67305710e9dfbbd5ff5902411b8534a633a59bd48aed5b");
        assert_eq!(hex(color_record(22, 0, 0)), "cbf2f147574d2249756cc55b87bce8566e5862dc5eac025a59f024a75c087931");
        assert_eq!(hex(color_record(27, 26, 9942053)), "bacf36bc4af1c3963a0588d173411e98d9bb4336247d085503a2de6844f2c478");
    }
}
