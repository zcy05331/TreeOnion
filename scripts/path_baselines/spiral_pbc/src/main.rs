//! Spiral PIR (Blyss spiral-rs) behind the PBC cuckoo framework of the OnionPIR
//! cuckoo baseline: same root-excluded Merkle records, same 3-hash buckets and
//! bucket positions, same trial leaves; only the per-bucket PIR backend differs.
//! Every bucket shares one Spiral parameter set and one public-parameter upload.

mod pbc;

use pbc::*;
use serde_json::{json, Value};
use spiral_rs::aligned_memory::AlignedMemory64;
use spiral_rs::client::{Client, PublicParameters, Query};
use spiral_rs::params::Params;
use spiral_rs::poly::{from_ntt, PolyMatrix, PolyMatrixNTT, PolyMatrixRaw};
use spiral_rs::server::{
    encode, expand_query, fold_ciphertexts, get_v_folding_neg, load_db_from_seek,
    multiply_reg_by_database, pack, process_query,
};
use spiral_rs::util::params_from_json_obj;
use std::io::Cursor;
use std::time::Instant;

const RECORD_BYTES: usize = 32;
const PLAINTEXT_ITEM_BYTES: usize = 8192; // n = 2, p = 256: 2 x 2 polys x 2048 x 8 bits
const UPSTREAM_REPO: &str = "https://github.com/blyssprivacy/sdk";
const UPSTREAM_REV: &str = "fdb7206517c249603b0c91b65f1f29f95272107c";

struct Args {
    height: usize,
    warmups: usize,
    trials: usize,
    trial_seed: u64,
    output: String,
    nu1: usize,
    max_nu2: usize,
}

fn parse_args() -> Args {
    let mut a = Args { height: 0, warmups: 3, trials: 8, trial_seed: 7, output: String::new(), nu1: 9, max_nu2: 6 };
    let argv: Vec<String> = std::env::args().collect();
    let mut i = 1;
    while i < argv.len() {
        let v = argv.get(i + 1).unwrap_or_else(|| panic!("{} needs a value", argv[i]));
        match argv[i].as_str() {
            "--height" => a.height = v.parse().unwrap(),
            "--warmup" => a.warmups = v.parse().unwrap(),
            "--trials" => a.trials = v.parse().unwrap(),
            "--trial-seed" => a.trial_seed = v.parse().unwrap(),
            "--output" => a.output = v.clone(),
            "--nu1" => a.nu1 = v.parse().unwrap(),
            "--max-nu2" => a.max_nu2 = v.parse().unwrap(),
            other => panic!("unknown argument {other}"),
        }
        i += 2;
    }
    assert!(a.height >= 4 && a.height <= 27, "--height must be in 4..=27");
    assert!(!a.output.is_empty(), "--output is required");
    assert!(a.trials > 0, "--trials must be positive");
    a
}

fn ceil_log2(x: usize) -> usize {
    if x <= 1 { 0 } else { (usize::BITS - (x - 1).leading_zeros()) as usize }
}

/// Spiral's validated 2^20 x 256 B lattice parameters (CFG_20_256 in spiral-rs),
/// with nu_2 sized to the largest bucket. Smaller nu_2 only removes folding
/// steps, so noise stays within that configuration's budget; instances are
/// doubled instead of raising nu_2 above max_nu2.
fn spiral_shape(max_members: usize, nu1: usize, max_nu2: usize) -> (usize, usize) {
    for instances in [1usize, 2, 4, 8] {
        let per_item = PLAINTEXT_ITEM_BYTES * instances / RECORD_BYTES;
        let items = (max_members + per_item - 1) / per_item;
        let nu2 = std::cmp::max(1, ceil_log2(items).saturating_sub(nu1));
        if nu2 <= max_nu2 {
            return (nu2, instances);
        }
    }
    panic!("no Spiral shape within max_nu2 = {max_nu2}");
}

fn spiral_params_json(nu1: usize, nu2: usize, instances: usize) -> Value {
    json!({
        "n": 2, "nu_1": nu1, "nu_2": nu2, "p": 256, "q2_bits": 20,
        "t_gsw": 8, "t_conv": 4, "t_exp_left": 8, "t_exp_right": 56,
        "instances": instances, "db_item_size": PLAINTEXT_ITEM_BYTES * instances
    })
}

#[derive(Default, Clone, Copy)]
struct Stages {
    expand_ns: u128,
    first_dim_ns: u128,
    fold_ns: u128,
    pack_ns: u128,
    encode_ns: u128,
}

impl Stages {
    fn add(&mut self, o: &Stages) {
        self.expand_ns += o.expand_ns;
        self.first_dim_ns += o.first_dim_ns;
        self.fold_ns += o.fold_ns;
        self.pack_ns += o.pack_ns;
        self.encode_ns += o.encode_ns;
    }
    fn to_json(&self) -> Value {
        let ms = |x: u128| x as f64 / 1e6;
        json!({"expand": ms(self.expand_ns), "first_dim": ms(self.first_dim_ns), "fold": ms(self.fold_ns),
               "pack": ms(self.pack_ns), "encode": ms(self.encode_ns)})
    }
}

/// Upstream process_query with stage timers; the trial checks it is byte-identical.
fn process_query_timed(params: &Params, pp: &PublicParameters, query: &Query, db: &[u64]) -> (Vec<u8>, Stages) {
    let mut st = Stages::default();
    let t = Instant::now();
    let dim0 = 1 << params.db_dim_1;
    let num_per = 1 << params.db_dim_2;
    let db_slice_sz = dim0 * num_per * params.poly_len;
    let v_packing = pp.v_packing.as_ref();
    assert!(params.expand_queries, "the adapter always uses query expansion");
    let (v_reg_reoriented, v_folding) = expand_query(params, pp, query);
    let v_folding_neg = get_v_folding_neg(params, &v_folding);
    st.expand_ns += t.elapsed().as_nanos();

    let mut v_packed_ct: Vec<PolyMatrixRaw> = Vec::with_capacity(params.instances);
    for instance in 0..params.instances {
        let mut intermediate = Vec::with_capacity(num_per);
        let mut intermediate_raw = Vec::with_capacity(num_per);
        for _ in 0..num_per {
            intermediate.push(PolyMatrixNTT::zero(params, 2, 1));
            intermediate_raw.push(PolyMatrixRaw::zero(params, 2, 1));
        }
        let mut v_ct = Vec::new();
        for trial in 0..(params.n * params.n) {
            let idx = (instance * (params.n * params.n) + trial) * db_slice_sz;
            let cur_db = &db[idx..(idx + db_slice_sz)];
            let t = Instant::now();
            multiply_reg_by_database(&mut intermediate, cur_db, v_reg_reoriented.as_slice(), params, dim0, num_per);
            st.first_dim_ns += t.elapsed().as_nanos();
            let t = Instant::now();
            for i in 0..intermediate.len() {
                from_ntt(&mut intermediate_raw[i], &intermediate[i]);
            }
            fold_ciphertexts(params, &mut intermediate_raw, &v_folding, &v_folding_neg);
            st.fold_ns += t.elapsed().as_nanos();
            v_ct.push(intermediate_raw[0].clone());
        }
        let t = Instant::now();
        let packed_ct = pack(params, &v_ct, &v_packing);
        v_packed_ct.push(packed_ct.raw());
        st.pack_ns += t.elapsed().as_nanos();
    }
    let t = Instant::now();
    let out = encode(params, &v_packed_ct);
    st.encode_ns += t.elapsed().as_nanos();
    (out, st)
}

fn mean(xs: &[f64]) -> f64 {
    xs.iter().sum::<f64>() / xs.len() as f64
}

fn main() {
    let args = parse_args();
    let h = args.height;
    let leaf_count = 1usize << h;
    let items = item_count(h);
    let cp = benchmark_cuckoo_params(items, h);

    let setup_start = Instant::now();
    let buckets = build_buckets(&cp);
    let member_counts: Vec<usize> = buckets.iter().map(|b| b.len()).collect();
    let max_members = *member_counts.iter().max().unwrap();
    let total_members: usize = member_counts.iter().sum();
    let (nu2, instances) = spiral_shape(max_members, args.nu1, args.max_nu2);
    let params_json = spiral_params_json(args.nu1, nu2, instances);
    let params: Params = params_from_json_obj(&params_json);
    let per_item = params.db_item_size / RECORD_BYTES;
    let num_items = 1usize << (params.db_dim_1 + params.db_dim_2);
    assert!(num_items * per_item >= max_members);
    eprintln!("spiral_pbc: H={h} buckets={} max_members={max_members} nu1={} nu2={} instances={} items/bucket={num_items}",
              cp.num_buckets, params.db_dim_1, params.db_dim_2, params.instances);

    // Server: one Spiral database per bucket, members packed in rank order.
    let mut dbs: Vec<AlignedMemory64> = Vec::with_capacity(cp.num_buckets);
    for bucket in &buckets {
        let mut bytes = vec![0u8; num_items * params.db_item_size];
        for (pos, &ord) in bucket.iter().enumerate() {
            let (level, local) = node_of_ordinal(ord as usize);
            let off = (pos / per_item) * params.db_item_size + (pos % per_item) * RECORD_BYTES;
            bytes[off..off + RECORD_BYTES].copy_from_slice(&merkle_node(level, local));
        }
        dbs.push(load_db_from_seek(&params, &mut Cursor::new(bytes)));
    }
    let db_words: usize = dbs.iter().map(|d| d.len()).sum();

    // Client: one key set for every bucket (identical parameters).
    let mut client = Client::init(&params);
    let pp_client = client.generate_keys();
    let pp_bytes = pp_client.serialize();
    let pp_server = PublicParameters::deserialize(&params, &pp_bytes);
    let offline_setup_ms = setup_start.elapsed().as_secs_f64() * 1e3;

    let (warm, measured) = trial_plan(leaf_count, args.warmups, args.trials, args.trial_seed);
    let mut samples: Vec<Value> = Vec::new();
    let mut checked_upstream = false;
    let mut shapes: Option<(usize, usize)> = None;
    for (phase, leaves) in [("warmup", &warm), ("measured", &measured)] {
        for (trial, &leaf) in leaves.iter().enumerate() {
            let t = Instant::now();
            let ordinals = sibling_ordinals(leaf, h);
            let assignment = cuckoo_place(&ordinals, &cp);
            let routing_ms = t.elapsed().as_secs_f64() * 1e3;
            let (mut client_ms, mut server_ms, mut decode_ms) = (0f64, 0f64, 0f64);
            let (mut q_bytes, mut r_bytes, mut recovered) = (0usize, 0usize, 0usize);
            let mut stages = Stages::default();
            for b in 0..cp.num_buckets {
                let pos = match assignment[b] { Some(o) => position_in_bucket(&buckets[b], o), None => 0 };
                let t = Instant::now();
                let query = client.generate_query(pos / per_item);
                let wire = query.serialize();
                client_ms += t.elapsed().as_secs_f64() * 1e3;
                q_bytes += wire.len();
                let server_query = Query::deserialize(&params, &wire);

                let t = Instant::now();
                let (response, st) = process_query_timed(&params, &pp_server, &server_query, dbs[b].as_slice());
                server_ms += t.elapsed().as_secs_f64() * 1e3;
                stages.add(&st);
                r_bytes += response.len();
                if !checked_upstream {
                    // Instrumented path must match upstream process_query byte for byte.
                    let upstream = process_query(&params, &pp_server, &server_query, dbs[b].as_slice());
                    assert!(upstream == response, "instrumented process_query diverges from upstream");
                    checked_upstream = true;
                }

                let t = Instant::now();
                let item = client.decode_response(&response);
                decode_ms += t.elapsed().as_secs_f64() * 1e3;
                if let Some(o) = assignment[b] {
                    let off = (pos % per_item) * RECORD_BYTES;
                    let (level, local) = node_of_ordinal(o as usize);
                    assert!(item[off..off + RECORD_BYTES] == merkle_node(level, local),
                            "Spiral+PBC record mismatch at leaf {leaf} bucket {b}");
                    recovered += 1;
                }
            }
            assert_eq!(recovered, h, "every sibling recovered in the batch");
            match shapes {
                None => shapes = Some((q_bytes, r_bytes)),
                Some(s) => assert_eq!(s, (q_bytes, r_bytes), "communication shape changed between trials"),
            }
            if phase == "measured" {
                samples.push(json!({
                    "trial": trial, "leaf": leaf, "routing_ms": routing_ms, "client_query_ms": client_ms,
                    "server_total_ms": server_ms, "client_decode_ms": decode_ms, "query_bytes": q_bytes,
                    "response_bytes": r_bytes, "stages_ms": stages.to_json(), "correct": true
                }));
            }
        }
    }

    let (q_bytes, r_bytes) = shapes.unwrap();
    let pick = |k: &str| samples.iter().map(|s| s[k].as_f64().unwrap()).collect::<Vec<f64>>();
    let stage_mean = |k: &str| mean(&samples.iter().map(|s| s["stages_ms"][k].as_f64().unwrap()).collect::<Vec<f64>>());
    let report = json!({
        "schema_version": "spiral_pbc_profile.v1",
        "scheme": "spiral_pbc",
        "upstream": {"repository": UPSTREAM_REPO, "commit": UPSTREAM_REV, "crate": "lib/spiral-rs"},
        "path_semantics": "legacy_merkle_paths_omit_root",
        "height": h, "leaf_count": leaf_count, "item_count": items, "record_bytes": RECORD_BYTES,
        "actual_logical_bytes": items * RECORD_BYTES,
        "warmups": args.warmups, "measured_trials": args.trials, "trial_seed": args.trial_seed,
        "warmup_leaves": warm, "measured_leaves": measured,
        "pbc": {
            "framework": "same buckets, hashes, positions and leaf schedule as the OnionPIR cuckoo baseline",
            "batch_size": cp.batch_size, "num_buckets": cp.num_buckets, "num_hashes": cp.num_hashes,
            "hash_seed": cp.seed, "bucket_sizing": "max(ceil(1.5k), k+1) + 1",
            "bucket_member_counts": member_counts, "max_bucket_members": max_members,
            "total_member_entries": total_members, "dummy_query_position": 0
        },
        "spiral_parameters": {
            "json": params_json, "records_per_item": per_item, "items_per_bucket": num_items,
            "source": "spiral-rs CFG_20_256 lattice parameters with nu_2 sized to the largest bucket",
            "max_nu2": args.max_nu2
        },
        "setup": {
            "offline_setup_ms": offline_setup_ms,
            "preprocessed_db_bytes": db_words * 8,
            "logical_replicated_bytes": total_members * RECORD_BYTES,
            "padded_bucket_bytes": cp.num_buckets * num_items * params.db_item_size
        },
        "communication": {
            "public_parameter_bytes": pp_bytes.len(),
            "public_parameter_kind": "actual PublicParameters::serialize, uploaded once per session and shared by all buckets",
            "query_bytes_per_path": q_bytes, "query_kind": "actual Query::serialize summed over all buckets",
            "response_bytes_per_path": r_bytes, "response_kind": "actual process_query output summed over all buckets"
        },
        "timing_boundaries": {
            "server_total_ms": "sum over buckets of the instrumented process_query (expand, first_dim, fold, pack, encode)",
            "client_query_ms": "generate_query + serialize, summed over buckets",
            "client_decode_ms": "decode_response, summed over buckets; record comparison is outside the timer",
            "threads": "rayon pool; run with RAYON_NUM_THREADS=1 and one pinned CPU"
        },
        "samples": samples,
        "measured_summary": {
            "count": args.trials,
            "server_ms_mean": mean(&pick("server_total_ms")),
            "client_query_ms_mean": mean(&pick("client_query_ms")),
            "client_decode_ms_mean": mean(&pick("client_decode_ms")),
            "stages_ms_mean": {"expand": stage_mean("expand"), "first_dim": stage_mean("first_dim"),
                               "fold": stage_mean("fold"), "pack": stage_mean("pack"), "encode": stage_mean("encode")},
            "all_correct": true
        }
    });
    std::fs::write(&args.output, serde_json::to_string_pretty(&report).unwrap() + "\n").unwrap();
    eprintln!("spiral_pbc: H={h} mean server {:.1} ms, query {} B, response {} B, public params {} B",
              mean(&pick("server_total_ms")), q_bytes, r_bytes, pp_bytes.len());
}
