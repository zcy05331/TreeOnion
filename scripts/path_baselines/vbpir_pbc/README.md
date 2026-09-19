# VBPIR+PBC path adapter

This adapter wraps the TreePIR fork of VBPIR_PBC at upstream commit
`930063c5aefc441244abb4890fdf35383f3aa956`. The upstream source is staged under
the ignored `.benchmarks/vbpir-pbc` directory and receives only the narrow hooks
needed by the path harness: synthetic raw database input, cuckoo bucket key
exposure for exact full-path verification, and per-stage timing accessors.

Recreate the staged source from an existing TreePIR checkout:

```
scripts/path_baselines/vbpir_pbc/stage_vbpir_pbc.sh \
  ../generic-superrecord/.benchmarks/TreePIR
```

Build the adapter with the default staged source and local SEAL install:

```
cmake -S scripts/path_baselines/vbpir_pbc -B /tmp/vbpir-path-build
cmake --build /tmp/vbpir-path-build -j2
```

To use another staged source directory:

```
cmake -S scripts/path_baselines/vbpir_pbc -B /tmp/vbpir-path-build \
  -DVBPIR_ROOT=/absolute/path/to/VBPIR_PBC \
  -DSEAL_DIR=/absolute/path/to/seal/lib/cmake/SEAL-4.0
```

Run a local smoke:

```
/tmp/vbpir-path-build/vbpir_path_adapter \
  --height 8 --leaf-file leaves.txt --warmup 1 --trials 2 \
  --first-dim 128 --output result.json
```

`--first-dim` is a power-of-two VBPIR first-dimension override. If omitted, the
adapter keeps the upstream reference value `64`; formal H24 pilots should sweep
candidate values such as `128`, `256`, and `512` and keep the fastest
correctness-valid result.
