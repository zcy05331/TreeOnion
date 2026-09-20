//! Respire adapter for the shared full ancestor path workload.
mod cli;
mod params;
mod report;
mod runner;
mod workload;

use clap::Parser;

fn main() {
    let args = cli::Args::parse();
    let json = match args.height {
        8 => runner::execute::<params::SmokeH8>(
            &args,
            params::SMOKE_H8_SHAPE,
            params::SmokeH8::params_query_size(),
            params::SmokeH8::params_response_info().0,
        ),
        24 => runner::execute::<params::FormalH24>(
            &args,
            params::FORMAL_H24_SHAPE,
            params::FormalH24::params_query_size(),
            params::FormalH24::params_response_info().0,
        ),
        26 => runner::execute::<params::FormalH26>(
            &args,
            params::FORMAL_H26_SHAPE,
            params::FormalH26::params_query_size(),
            params::FormalH26::params_response_info().0,
        ),
        _ => Err(
            "supported heights are 8 for local smoke and 24/26 for formal Respire path runs"
                .to_string(),
        ),
    };
    match json.and_then(|contents| cli::write_output(&args.output, &contents)) {
        Ok(()) => {}
        Err(err) => {
            eprintln!("path_workload: {err}");
            std::process::exit(2);
        }
    }
}
