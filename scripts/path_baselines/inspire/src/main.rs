mod backend;
mod cli;
mod database;
mod execution;
mod report;
mod util;
mod workload;

use anyhow::{Result, anyhow, bail};
use poulpy_pir::{
    config::{Collapse, Config},
    payload::Payload,
};

use crate::{
    backend::P,
    cli::parse_args,
    database::build_level,
    execution::run_iteration,
    report::{build_report, write_json},
    workload::{checked_pow2, read_leaf_file},
};

fn main() -> Result<()> {
    let args = parse_args()?;
    let leaves = read_leaf_file(&args.leaf_file)?;
    let needed = args
        .warmup
        .checked_add(args.trials)
        .ok_or_else(|| anyhow!("warmup + trials overflows usize"))?;
    validate_schedule(args.height, args.trials, needed, &leaves)?;

    let config = Config::<P>::with_collapse(Collapse::Interpolation);
    let column_height = config.column_height();
    let n = config.n();
    let payloads_per_column = column_height / P::EXPONENT;

    let mut levels = Vec::with_capacity(args.height as usize + 1);
    for level in 0..=args.height {
        eprintln!("preparing InsPIRe level {level}/{}", args.height);
        levels.push(build_level(level, n, column_height, payloads_per_column)?);
    }

    let mut results = Vec::with_capacity(needed);
    for (i, &leaf_id) in leaves.iter().take(needed).enumerate() {
        let phase = if i < args.warmup {
            "warmup"
        } else {
            "measured"
        };
        results.push(run_iteration(
            i + 1,
            phase,
            args.height,
            leaf_id,
            &mut levels,
        )?);
        eprintln!("completed {phase} path {} leaf {leaf_id}", i + 1);
    }

    let level_reports = levels.into_iter().map(|level| level.report).collect();
    let report = build_report(&args, level_reports, results)?;
    write_json(&args.output, &report)
}

fn validate_schedule(height: u32, trials: usize, needed: usize, leaves: &[u64]) -> Result<()> {
    if trials == 0 {
        bail!("--trials must be greater than zero");
    }
    if leaves.len() < needed {
        bail!(
            "leaf file has {} ids but warmup+trials requires {needed}",
            leaves.len()
        );
    }
    let leaf_count = checked_pow2(height)?;
    for &leaf in leaves.iter().take(needed) {
        if leaf >= leaf_count {
            bail!("leaf id {leaf} is out of range for height {height} (expected < {leaf_count})");
        }
    }
    Ok(())
}
