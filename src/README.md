# Source map

The public C++ interfaces live in `includes/`. Implementations are grouped by
the work they perform; CMake discovers all `.cpp` files under this directory.
Reconfigure the build after adding or removing a translation unit.

## Standard PIR

| Files | Responsibility |
| --- | --- |
| `main.cpp` | Command-line parsing and dispatch to the tests |
| `pir.cpp`, `includes/pir.h`, `includes/database_constants.h` | Scheme identity, parameter selection and runtime database/query shapes |
| `client.cpp` | Client construction, secret/evaluation keys and shared sessions |
| `client_query.cpp` | Index decomposition, packed query generation and fresh zero ciphertexts |
| `client_response.cpp` | Response wire decoding, decryption and noise diagnostics |
| `server.cpp` | Database allocation/loading, session registration and plaintext verification records |
| `server_first_dim.cpp` | Query preparation, first-dimension matrix evaluation and CRT/NTT finalization |
| `server_query.cpp` | Query expansion, selector completion, CMux reduction and full-query orchestration |
| `server_response.cpp` | Response modulus switching and wire encoding |

`PirServer::make_query_profiled` is the online pipeline entry. Its phase timers
attribute server time to the pipeline stages.

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

## Tree paths and h=1 multi-plane entries

The legacy path is described by `tree_index.h`. Its query, rotation,
projection and selection kernels have matching `tree_*.cpp` files.
`tree_database.cpp` constructs the reference and optimized database views;
`tree_select.cpp` performs online alpha preparation and level selection. Both
use the interfaces in `tree_select.h`. These files provide the low-level
gamma/alpha/beta coordinate system and homomorphic kernels reused by the final
h=1 entry layout.

The primary h=1 multi-plane path is in `tree_multiplane_database.cpp` and
`tree_multiplane_response.cpp`, with its contract in `includes/tree_multiplane.h`.
It reuses the old gamma/alpha/beta coordinates and per-plane selection, shares
query preparation across planes, and pools level/plane fragments with either
Projection or MLWE. Its g/P choice does not change the node-to-polynomial
assignment.

| Module | Responsibility |
| --- | --- |
| `tree_entry_codec` | h=1 entry bitstream representation, per-entry plane packing, zero padding and final g/P policy |
| `tree_packing` | Shared response placement, path chunk partitioning and MLWE residue merge |
| `tree_multiplane_*` | Final old-layout h=1 database packing, in-place node updates, serving and decoding |
| `query_wire` | Query wire format: a packed query travels as its seed and c0 |

The final structural policy is implemented by
`tree_entry_codec::choose_layout(E, c, n)`: among dyadic
`g | n` with `rho=n/g >= 2` and `P=ceil(E/(c*g)) <= 6`, it maximizes payload
utilization and breaks ties by smaller `P`. For the paper parameter point
`c=12,n=2048`, this gives 16B g4/P3, 32B g8/P3, 48B g32/P1, 64B g16/P3 and
128B g32/P3.

## Tests

`tests/run_test.cpp` registers the stable `--test` names. `--test all` runs the
correctness tests in their declared order and reports inapplicable parameter
points separately. Long test entries are organized into named scenarios; they
retain the shared fixtures and assertion order.
