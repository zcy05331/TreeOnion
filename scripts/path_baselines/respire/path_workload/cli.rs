use clap::Parser;
use std::fs;
use std::io::{self, Write};
use std::path::PathBuf;

#[derive(Parser, Debug)]
pub struct Args {
    #[arg(long)]
    pub height: usize,
    #[arg(long)]
    pub leaf_file: PathBuf,
    #[arg(long)]
    pub warmup: usize,
    #[arg(long)]
    pub trials: usize,
    #[arg(long, default_value = "-")]
    pub output: String,
}

pub fn write_output(path: &str, contents: &str) -> Result<(), String> {
    if path == "-" {
        io::stdout()
            .lock()
            .write_all(contents.as_bytes())
            .map_err(|err| format!("cannot write stdout: {err}"))
    } else {
        fs::write(path, contents).map_err(|err| format!("cannot write {path}: {err}"))
    }
}
