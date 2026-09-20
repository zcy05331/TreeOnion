use crate::cli::Args;
use crate::params::RespireShape;
use crate::report;
use crate::workload::{make_plans, read_leaves, record_for, TrialPlan};
use respire::pir::pir::{PIRRecordBytes, Stats, PIR};
use std::time::{Duration, Instant};

pub struct TrialResult {
    pub warmup: bool,
    pub leaf: usize,
    pub path_nodes: Vec<usize>,
    pub request_indices: Vec<usize>,
    pub query_ns: u128,
    pub server_ns: u128,
    pub decode_ns: u128,
    pub correct_entries: usize,
    pub verified_dummy_entries: usize,
    pub total_verified_entries: usize,
    pub stages: Vec<(&'static str, u128)>,
}

fn stats_ns(stats: &Stats<Duration>) -> Vec<(&'static str, u128)> {
    stats
        .as_vec()
        .into_iter()
        .map(|(name, value)| (name, value.as_nanos()))
        .collect()
}

fn run<P: PIR>(height: usize, plans: &[TrialPlan]) -> (Vec<TrialResult>, u128)
where
    P::RecordBytes: PIRRecordBytes,
{
    let setup_begin = Instant::now();
    let (db, hint) = P::encode_db(|i| record_for::<P>(height, i), None);
    let (qk, pp) = P::setup(None);
    let setup_ns = setup_begin.elapsed().as_nanos();

    let mut results = Vec::with_capacity(plans.len());
    for plan in plans {
        let mut query_stats = Stats::new();
        let mut answer_stats = Stats::new();
        let mut decode_stats = Stats::new();

        let query_begin = Instant::now();
        let (query, state) = P::query(&qk, &plan.request_indices, &hint, Some(&mut query_stats));
        let query_ns = query_begin.elapsed().as_nanos();

        let server_begin = Instant::now();
        let response = P::answer(&pp, &db, &query, None, Some(&mut answer_stats));
        let server_ns = server_begin.elapsed().as_nanos();

        let decode_begin = Instant::now();
        let decoded = P::extract(&qk, &response, &state, Some(&mut decode_stats));
        let decode_ns = decode_begin.elapsed().as_nanos();

        let mut total_verified_entries = 0usize;
        for (position, &record_index) in plan.request_indices.iter().enumerate() {
            let expected = record_for::<P>(height, record_index);
            assert_eq!(
                decoded[position].as_bytes(),
                expected.as_bytes(),
                "leaf {} returned entry {} record {}",
                plan.leaf,
                position,
                record_index
            );
            total_verified_entries += 1;
        }
        let correct_entries = plan.path_nodes.len();
        let verified_dummy_entries = total_verified_entries - correct_entries;
        let stages = stats_ns(&answer_stats);
        let stage_sum: u128 = stages.iter().map(|(_, ns)| *ns).sum();
        assert!(
            stage_sum <= server_ns,
            "exclusive Respire answer stages exceeded full PIR::answer"
        );

        eprintln!(
            "completed Respire path {} leaf {}",
            results.len() + 1,
            plan.leaf
        );
        results.push(TrialResult {
            warmup: plan.warmup,
            leaf: plan.leaf,
            path_nodes: plan.path_nodes.clone(),
            request_indices: plan.request_indices.clone(),
            query_ns,
            server_ns,
            decode_ns,
            correct_entries,
            verified_dummy_entries,
            total_verified_entries,
            stages,
        });
    }
    (results, setup_ns)
}

pub fn execute<P: PIR>(
    args: &Args,
    shape: RespireShape,
    query_bytes: usize,
    response_bytes: usize,
) -> Result<String, String>
where
    P::RecordBytes: PIRRecordBytes,
{
    if args.trials == 0 {
        return Err("--trials must be greater than zero".to_string());
    }
    if P::NUM_RECORDS != (1usize << (args.height + 1)) {
        return Err("compiled Respire backend size does not match requested height".to_string());
    }
    let leaves = read_leaves(&args.leaf_file, args.warmup + args.trials, args.height)?;
    let plans = make_plans::<P>(args.height, &leaves, args.warmup);
    let (results, setup_ns) = run::<P>(args.height, &plans);
    Ok(report::render::<P>(
        args,
        shape,
        setup_ns,
        &results,
        query_bytes,
        response_bytes,
    ))
}
