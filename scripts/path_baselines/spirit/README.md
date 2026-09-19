# SPIRIT path baseline adapter

This adapter runs the public SPIRIT batch PIR implementation against the full binary-tree path workload. It uses the real `BatchDBSetup`, `BatchQuery`, `BatchAnswerPrecompute`, `BatchAnswerQueryPrecompute`, `BatchAnswer`, and `BatchExtract` API from `https://github.com/zhouzhangwalker/SPIRIT` for plain SPIRIT. With `--scheme spirit_rc`, it uses the same upstream batch query/answer pipeline, then applies the adapter RC module's encrypted response rotation and MultiResMerge routine.

The workload database is the full BFS tree with 32-byte records. A path query contains the root and every ancestor down to the requested leaf, so height `H` returns `H + 1` records. Flat BFS indices follow:

```text
flat(level, node) = (1 << level) - 1 + node
node(level, leaf) = leaf >> (H - level)
```

The adapter never enables SPIRIT's `benchmark_randomize_hint_matrix` mode because upstream documents that mode as timing-only and not correctness-valid. If the path batch is padded to a power of two, the public dummy indices are included in query, server, response, and decode cost, and their decoded records are verified too. The reported `correct_entries` count is the number of real path entries. A decoded-record mismatch throws and exits nonzero before a JSON report is written.

The instrumented build keeps SPIRIT's public checkout unchanged and patches only the generated build copy. It exposes the existing `BatchAnswer` timers for `secret_unpack`, `matvec`, `modswitch`, `b_extract`, and `lwe_pack`, plus `answer_glue` for remaining measured answer wall time. The JSON also reports actual serialized query/response byte counts and the minimum SEAL invariant noise budget over returned response ciphertexts. `backend_records` is the realized bucket-matrix capacity in 32-byte-equivalent records after PBC replication and row/column padding, not the logical tree size. The `layout` object separately reports physical copied PBC entries and byte-cell capacity. `setup.helper_bytes` is `null` because this adapter does not have an actual setup serializer. The separate `setup.public_metadata_bytes` field is a labeled current in-memory model for PBC routing tables, bucket-layout metadata, and public matrix seeds; it is not claimed as required wire communication because these tables may be reconstructed from public seeds/layout. Query and response bytes remain actual serialized payload sizes.

## Plain SPIRIT vs SPIRIT+RC

`--scheme spirit` uses upstream's balanced bucket layout and returns all bucket response ciphertexts. `--scheme spirit_rc` uses a public strided layout required by response compression: for polynomial degree `n` and 32-byte records, each bucket stores byte `j` of a record at row `(slot mod (n/32)) + j * (n/32)`. This forces one response polynomial per bucket and lets the RC module privately rotate the selected residue to zero and merge all bucket residues into one ciphertext.

The RC implementation is a prototype baseline of the paper's response-side idea with packed selector upload enabled. The client packs the selected-residue bits into one RNS-RGSW selector query ciphertext and the server expands it before private rotation. The JSON counts the packed selector ciphertext and expansion parameters in `query_rc_selector_bytes`, and counts merge, expansion, and relinearization keys in `query_rc_galois_key_bytes`. The server selector expansion time is reported as the `selector_unpack` stage.

The two schemes therefore compare complete supported pipelines with different public database geometry. They are not a fixed-layout ablation.

## Parameter lineage

The adapter uses the public SPIRIT artifact parameter line currently exercised by the upstream code path: BFV response context `n=2048`, coefficient-modulus bit sizes `[56,60]`, plaintext modulus `65537`, LWE modulus `2^32`, and default LWE dimension `1024`. Packed selector expansion uses an auxiliary context at the same degree with coefficient-modulus bits `[56,60,60]` and primes `72057594037641217`, `1152921504606830593`, and `1152921504606683137`. SEAL rejects these degree/modulus pairs at the standard `tc128` security gate, so the adapter constructs contexts with `sec_level_type::none` and reports `seal_security_level: "none"` plus the actual SEAL coefficient modulus primes in JSON. This is an artifact-compatibility setting, not a 128-bit security claim.

Build with the SEAL fork bundled in the pinned SPIRIT checkout. A system SEAL
package cannot replace it because SPIRIT uses additional offline/online APIs.
On x86 Linux, add `-DSEAL_USE_INTEL_HEXL=ON` to build the fork's pinned HEXL dependency.

Build:

```bash
cmake -S scripts/path_baselines/spirit -B .benchmarks/build-spirit-local \
  -DSPIRIT_DIR="$PWD/.benchmarks/SPIRIT"
cmake --build .benchmarks/build-spirit-local -j2
```

Run plain SPIRIT:

```bash
.benchmarks/build-spirit-local/spirit_path_adapter \
  --scheme spirit \
  --height 8 \
  --leaf-file /path/to/leaves.txt \
  --warmup 1 \
  --trials 2 \
  --output /tmp/spirit-path.json
```

Run SPIRIT+RC:

```bash
.benchmarks/build-spirit-local/spirit_path_adapter \
  --scheme spirit_rc \
  --height 8 \
  --leaf-file /path/to/leaves.txt \
  --warmup 1 \
  --trials 2 \
  --output /tmp/spirit-rc-path.json
```

See `RC.md` for the RC layout, algebra, operation counts, and dense-polynomial negative-control tests.
