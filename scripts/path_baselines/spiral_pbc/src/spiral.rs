//! Spiral PIR (Blyss spiral-rs) pieces shared by the two batch codes: one
//! parameter set sized to the largest database, and upstream process_query
//! with stage timers.

use serde_json::{json, Value};
use spiral_rs::client::{PublicParameters, Query};
use spiral_rs::params::Params;
use spiral_rs::poly::{from_ntt, PolyMatrix, PolyMatrixNTT, PolyMatrixRaw};
use spiral_rs::server::{
    encode, expand_query, fold_ciphertexts, get_v_folding_neg, multiply_reg_by_database, pack,
};
use std::time::Instant;

pub const RECORD_BYTES: usize = 32;
pub const PLAINTEXT_ITEM_BYTES: usize = 8192; // n = 2, p = 256: 2 x 2 polys x 2048 x 8 bits
pub const UPSTREAM_REPO: &str = "https://github.com/blyssprivacy/sdk";
pub const UPSTREAM_REV: &str = "fdb7206517c249603b0c91b65f1f29f95272107c";

pub fn ceil_log2(x: usize) -> usize {
    if x <= 1 { 0 } else { (usize::BITS - (x - 1).leading_zeros()) as usize }
}

/// Spiral's validated 2^20 x 256 B lattice parameters (CFG_20_256 in spiral-rs),
/// with nu_2 sized to the largest bucket. Smaller nu_2 only removes folding
/// steps, so noise stays within that configuration's budget; instances are
/// doubled instead of raising nu_2 above max_nu2.
pub fn spiral_shape(max_members: usize, nu1: usize, max_nu2: usize) -> (usize, usize) {
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

pub fn spiral_params_json(nu1: usize, nu2: usize, instances: usize) -> Value {
    json!({
        "n": 2, "nu_1": nu1, "nu_2": nu2, "p": 256, "q2_bits": 20,
        "t_gsw": 8, "t_conv": 4, "t_exp_left": 8, "t_exp_right": 56,
        "instances": instances, "db_item_size": PLAINTEXT_ITEM_BYTES * instances
    })
}

#[derive(Default, Clone, Copy)]
pub struct Stages {
    pub expand_ns: u128,
    pub first_dim_ns: u128,
    pub fold_ns: u128,
    pub pack_ns: u128,
    pub encode_ns: u128,
}

impl Stages {
    pub fn add(&mut self, o: &Stages) {
        self.expand_ns += o.expand_ns;
        self.first_dim_ns += o.first_dim_ns;
        self.fold_ns += o.fold_ns;
        self.pack_ns += o.pack_ns;
        self.encode_ns += o.encode_ns;
    }
    pub fn to_json(&self) -> Value {
        let ms = |x: u128| x as f64 / 1e6;
        json!({"expand": ms(self.expand_ns), "first_dim": ms(self.first_dim_ns), "fold": ms(self.fold_ns),
               "pack": ms(self.pack_ns), "encode": ms(self.encode_ns)})
    }
}

/// Upstream process_query with stage timers; the trial checks it is byte-identical.
pub fn process_query_timed(params: &Params, pp: &PublicParameters, query: &Query, db: &[u64]) -> (Vec<u8>, Stages) {
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

pub fn mean(xs: &[f64]) -> f64 {
    xs.iter().sum::<f64>() / xs.len() as f64
}
