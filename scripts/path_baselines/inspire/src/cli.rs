use std::path::PathBuf;

use anyhow::{Result, anyhow, bail};

#[derive(Debug)]
pub(crate) struct Args {
    pub(crate) height: u32,
    pub(crate) leaf_file: PathBuf,
    pub(crate) warmup: usize,
    pub(crate) trials: usize,
    pub(crate) output: PathBuf,
}

pub(crate) fn parse_args() -> Result<Args> {
    let mut height = None;
    let mut leaf_file = None;
    let mut warmup = None;
    let mut trials = None;
    let mut output = None;
    let mut it = std::env::args().skip(1);
    while let Some(arg) = it.next() {
        match arg.as_str() {
            "--height" => height = Some(parse_next(&mut it, "--height")?),
            "--leaf-file" => leaf_file = Some(PathBuf::from(next_value(&mut it, "--leaf-file")?)),
            "--warmup" => warmup = Some(parse_next(&mut it, "--warmup")?),
            "--trials" => trials = Some(parse_next(&mut it, "--trials")?),
            "--output" => output = Some(PathBuf::from(next_value(&mut it, "--output")?)),
            "-h" | "--help" => usage(),
            _ => bail!("unknown argument {arg:?}"),
        }
    }
    let height = height.ok_or_else(|| anyhow!("missing --height"))?;
    if !(1..=30).contains(&height) {
        bail!("--height {height} is outside the supported range 1..=30");
    }
    Ok(Args {
        height,
        leaf_file: leaf_file.ok_or_else(|| anyhow!("missing --leaf-file"))?,
        warmup: warmup.unwrap_or(0),
        trials: trials.ok_or_else(|| anyhow!("missing --trials"))?,
        output: output.ok_or_else(|| anyhow!("missing --output"))?,
    })
}

fn usage() -> ! {
    eprintln!(
        "usage: inspire-path-adapter --height H --leaf-file PATH --warmup W --trials N --output JSON"
    );
    std::process::exit(2);
}

fn next_value(it: &mut impl Iterator<Item = String>, name: &str) -> Result<String> {
    it.next().ok_or_else(|| anyhow!("{name} requires a value"))
}

fn parse_next<T>(it: &mut impl Iterator<Item = String>, name: &str) -> Result<T>
where
    T: std::str::FromStr,
    T::Err: std::fmt::Display,
{
    let value = next_value(it, name)?;
    value
        .parse()
        .map_err(|err| anyhow!("{name} value {value:?} is invalid: {err}"))
}
