# Source map

The public C++ interfaces live in `includes/`. Implementations are grouped by
the work they perform; CMake discovers all `.cpp` files under this directory.
Reconfigure the build after adding or removing a translation unit.

## Standard PIR

| Files | Responsibility |
| --- | --- |
| `main.cpp` | Command-line parsing and dispatch to tests, layout sweeps and benchmark suites |
| `pir.cpp`, `includes/pir.h`, `includes/database_constants.h` | Scheme identity, parameter selection and runtime database/query shapes |
| `client.cpp` | Client construction, secret/evaluation keys and shared sessions |
| `client_query.cpp` | Index decomposition, packed query generation and fresh zero ciphertexts |
| `client_response.cpp` | Response wire decoding, decryption and noise diagnostics |
| `server.cpp` | Database allocation/loading, session registration and plaintext verification records |
| `server_first_dim.cpp` | Query preparation, first-dimension matrix evaluation and CRT/NTT finalization |
| `server_query.cpp` | Query expansion, selector completion, CMux reduction and full-query orchestration |
| `server_response.cpp` | Response modulus switching and wire encoding |

`PirServer::make_query_profiled` is the online pipeline entry. Its phase timers
surround the same operations exposed by the benchmark JSON writers.

## Cryptographic and arithmetic primitives

| Files | Responsibility |
| --- | --- |
| `rlwe.cpp` | Single-modulus and RNS RLWE encryption, decryption and ciphertext operations |
| `gsw.cpp` | GSW construction, decomposition, external products and selector completion |
| `bv_keyswitch.cpp` | Balanced gadget digits, key generation and K1/K2 key-switch implementations |
| `matrix.cpp` | Scalar and SIMD matrix kernels and dispatch decisions |
| `utils.cpp` | Integer/modular arithmetic, primes, gadgets, CRT and database shape arithmetic |
| `utils_ntt.cpp` | NTT cache/root registration, transforms and polynomial automorphisms/shifts |
| `utils_sampling.cpp` | Gaussian, uniform and ternary polynomial sampling |
| `utils_display.cpp` | Integer/plaintext display, plaintext comparison diagnostics and progress output |
| `includes/logging.h` | Process-wide experiment timers and phase aggregation |

The scalar and SIMD implementations remain explicit because their buffer
representations and arithmetic constraints differ.

## Tree paths and super-records

The legacy path is described by `tree_index.h`. Its query, rotation, projection,
response packing and ring compression stages have matching `tree_*.cpp` files.
`tree_database.cpp` constructs the reference and optimized database views;
`tree_select.cpp` performs online alpha preparation and level selection. Both
use the interfaces in `tree_select.h`.

The primary h=1 multi-plane path is in `tree_multiplane_database.cpp` and
`tree_multiplane_response.cpp`, with its contract in `includes/tree_multiplane.h`.
It reuses the old gamma/alpha/beta coordinates and per-plane selection, shares
query preparation across planes, and pools level/plane fragments with either
Projection or MLWE. Its g/P choice does not change the node-to-polynomial
assignment. The original single-plane path remains an independent baseline.

The generic path uses `tree_superrecord_*`:

| Module | Responsibility |
| --- | --- |
| `common`, `codec`, `index` | Entry representation, bit packing, coordinates and plan tags |
| `planner` | Feasible shapes, resource/work metrics, policy filtering and candidate ordering |
| `cost_model` | Calibration and predicted server cost |
| `query` | Tagged session keys, packed queries and selector expansion |
| `database` | Reference/M32 database preprocessing, storage accounting and updates |
| `select` | Alpha preparation and online first-dimension selection |
| `response` | Per-level execution, response placement, decoding and updates |
| `noise` | Measured checkpoints and target-shape admission policy |
| `tree_response_merge` | Shared response key switches for MLWE packing |

The experimental noise probe is separate from the correctness suite. It keeps
its checkpoint collection and measured pipeline together so each diagnostic
can be traced to the operation that produced it.

## Merkle and Cuckoo baselines

| Files | Responsibility |
| --- | --- |
| `merkle_baseline.cpp` | Node codec, flat/layer layouts and communication geometry |
| `merkle_benchmark.cpp` | Workloads, trial schedules, suite execution and layout sweeps |
| `merkle_benchmark_cases.cpp` | Standard, flat and layerwise encrypted benchmark cases |
| `merkle_benchmark_report.cpp` | Statistics, communication accounting, JSON writing and console reports |
| `includes/merkle_benchmark_internal.h` | Small implementation helpers shared by the benchmark translation units |
| `layer_layout_planner.cpp` | Candidate features, Pareto filtering and budget-constrained selection |
| `layer_layout_profile.cpp` | Profile JSON, environment matching and median validation |
| `cuckoo_batch.cpp` | Bucket construction and deterministic batch placement |

## Tests and benchmark entry points

`tests/run_test.cpp` registers the stable `--test` names. `--test all` runs the
correctness tests in their declared order and reports inapplicable parameter
points separately. Long test entries are organized into named scenarios; they
retain the shared fixtures and assertion order.

Timed entry points such as `tree_bench`, `cuckoo_bench` and
`tree_superrecord_bench` and `tree_multiplane_bench` are separate from the
in-process correctness suite.
The generic benchmark has separate option/report implementation files beside
its entry file in `tests/`; its prepared runners retain their databases and
sessions for the complete interleaved trial schedule.

The main end-to-end gate is `../scripts/run_regression.sh <binary>`. It runs the
C++ suite and then feeds real benchmark output to the Python validators,
selectors, analyzers and report writers. Python test discovery covers campaign
orchestration and report interpretation. The external SealPIR adapter has its
own CMake project and tests under `../scripts/treepir_sealpir/`.
