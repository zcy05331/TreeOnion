# Scripts and verification

Commands keep their existing paths. The executable used by real-binary Python
tests can be selected with `ONIONPIR_BENCH_BIN`; the default is
`build-x86_64-benchmark/Onion-PIR`.

## Local verification

```bash
bash scripts/run_regression.sh /path/to/Onion-PIR
ONIONPIR_BENCH_BIN=/path/to/Onion-PIR python3 -m unittest discover -s scripts/tests -v
TREEPIR_SEALPIR_ADAPTER=/path/to/treepir_sealpir_adapter \
  python3 -m unittest discover -s scripts/treepir_sealpir/tests -v
```

`run_regression.sh` separates the in-process suite, iteration accounting,
Merkle CLI contracts, CloudLab report chain, tree JSON checks, Python driver
contracts and argument rejection checks into named stages. A failed stage
keeps its original nonzero status and displays the last log before cleanup.

## Experiments

| Entry | Purpose |
| --- | --- |
| `../run.py` | Build a selected configuration and launch a named test or benchmark |
| `run_perf_suite.sh` | Build/run the selected baseline matrix, enforce resource gates and collect unit statuses |
| `run_superrecord_sweep.py` / `.sh` | Enumerate entry/workload tasks and record raw output plus provenance |
| `benchmark_gadget_configs.py` | Execute paired gadget configurations, enforce comparability and write the comparison |
| `benchmark_tree_multiplane.py` | Sweep old gamma/alpha/beta h=1 layouts over g/N0; derive P/b and rank by full measured server time |
| `run_entry_size_sweep.py` | Run or reuse entry-size cases and produce per-entry selection reports |
| `run_treepir_backend_tuning.py` | Prepare index schedules, evaluate backend pilots and run the selected formal candidate |
| `run_tree_t257_comparison.py` | Pair baseline/candidate plaintext-modulus runs under a fixed layout and schedule |
| `run_cloudlab_profile_matrix.sh` | Orchestrate case-specific profile runs and their validation |
| `read_process_peak_memory.py` | Record a child process's exit status, merged log and peak RSS |
| `collect_cloudlab_results.py` | Collect the campaign artifacts and status evidence |

The short `run_*after*`, `run_*interlude*` and Wisconsin shell commands retain
their specific campaign sequencing. They are independent entry points rather
than instances of a generic workflow engine.

## Interpretation and reports

- `summarize_cloudlab_profiles.py`: validate each schema, collect stage rows,
  aggregate samples, and write CSV/JSON/Markdown summaries.
- `analyze_superrecord_sweep.py`: classify a run and compare measured candidates.
- `select_cuckoo_layout.py`: validate matched pilots and select by mean server time.
- `summarize_layer_layout_profile.py`: explain selected per-level layouts and
  compare legacy/profiled reports.
- `analyze_wisconsin_profiles.py` and `plot_wisconsin_profiles.R`: produce the
  Wisconsin stage-analysis tables and plots.

The Python orchestration modules keep their imported names and module globals,
including the injection points used by unit tests. The scripts' internal
functions separate argument handling, execution, selection and report output.

## External adapters

`treepir_sealpir/` builds the existing SealPIRplus core adapter. The adapter
separates input/option parsing, encrypted execution and JSON reporting while
retaining the same transport operations and stage measurements. Its upstream
core is copied into the build directory by `patch_sealpir_core.py`.

`treepir_spiral/` contains preparation/build commands for the Spiral prototype.
`benchmarks/` contains upstream licenses, historical prototype patches and the
index verification helper. Archived inputs and results under `../outputs/`
remain separate from executable source.
