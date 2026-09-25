# Scripts and verification

The main branch keeps only the final h=1 old-index primary path and the retained
baseline families requested for comparison. The full pre-cleanup experiment
surface is preserved on `codex/archive-full-pir-20260920` at `fcc521f`.

## Verification

```bash
bash scripts/run_regression.sh /path/to/Onion-PIR
ONIONPIR_BENCH_BIN=/path/to/Onion-PIR \
RESPIRE_PATH_ADAPTER_BIN=/path/to/path_workload \
  python3 -m unittest discover -s scripts/tests -v
TREEPIR_SEALPIR_ADAPTER=/path/to/treepir_sealpir_adapter \
  python3 -m unittest discover -s scripts/treepir_sealpir/tests -v
```

`run_regression.sh` covers the retained surface: C++ `--test all`, Merkle and
Cuckoo benchmark JSON contracts, the h=1 primary driver smoke, and script unit
tests. Retired generic h3, standalone Tree g32/g1/compress, t257, VBPIR+PBC,
InsPIRe, TreePIR+Spiral and SPIRIT+RC paths are intentionally absent.

## Current experiment entry points

| Entry | Purpose |
| --- | --- |
| `../run.py` | Build a selected configuration and launch a named retained test or benchmark |
| `benchmark_tree_multiplane.py` | Sweep the final h=1 gamma/alpha/beta layout over N0 for a fixed entry size |
| `run_primary_matrix.py` | Primary h=1 N0 tuning and matched query-buffer measurements at all four sizes |
| `run_core_matrix.py` | StandardOnionPIR, Layerwise tuned and its merged/packed ablations, Cuckoo batch tuned and Flat over 256 MiB to 4 GiB, 8 GiB on request |
| `run_external_matrix.py` | Respire over the same heights, with resource admission |
| `run_spiral_matrix.py` | Spiral per bucket of the Cuckoo baseline's PBC instance, or per TreePIR color |
| `run_kspir_pbc_matrix.py` | KsPIR per bucket of the Cuckoo baseline's PBC instance |
| `run_via_matrix.py` | VIA-B batch PIR, and VIA per bucket of the same PBC instance |
| `run_treepir_matrix.py` | TreePIR VBPIR/SealPIR backend tuner wrapper |
| `run_treepir_backend_tuning.py` | Generate the shared Floyd leaf schedule, tune TreePIR backends, and run the selected candidate |
| `run_path_baselines.py` | Finite runner for an already-built Respire adapter |
| `select_cuckoo_layout.py` | Validate matched Cuckoo pilots and select by mean server time |
| `summarize_cloudlab_profiles.py` | Validate retained profile schemas and aggregate stage/communication rows |
| `summarize_layer_layout_profile.py` | Explain selected per-level Layerwise layouts |
| `read_process_peak_memory.py` | Record child exit status, merged log and peak RSS |

## External adapters

`path_baselines/respire/` owns the Respire path workload. The adapter supports
H8 smoke and formal H22/H23/H24/H26 points.

`path_baselines/spiral_pbc/`, `kspir_pbc/`, `via_pbc/` and `via_b/` own the
PBC-backed and VIA-B adapters; [path_baselines/README.md](path_baselines/README.md) lists
them.

`treepir_sealpir/` owns the TreePIR SealPIRplus adapter used by the retained
TreePIR backend tuner.
