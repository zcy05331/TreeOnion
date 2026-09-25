# Spiral + PBC cuckoo baseline

Spiral has no native batch mode, so it runs behind the same probabilistic batch
code as the OnionPIR cuckoo baseline. `src/pbc.rs` is a bit-exact port of
`src/cuckoo_batch.cpp`, the Merkle helpers and `make_benchmark_trial_plan`:
the same root-excluded Merkle records (2(2^H - 1) nodes of 32 bytes), the same
three splitmix hashes with seed `0x63756b32`, `max(ceil(1.5k), k+1) + 1` buckets
for a batch of k = H siblings, the same rank-in-bucket positions, the same
random-walk placement and the same seed-7 leaf schedule. The unit tests check
the H=22 bucket member counts and the H=22/H=24 leaf schedules against the C++
benchmark's output. Rows therefore differ from the OnionPIR cuckoo rows only in
the per-bucket PIR backend.

The backend is the complete spiral-rs in the Blyss SDK
(`blyssprivacy/sdk@fdb72065`, `lib/spiral-rs`); `menonsamir/spiral-rs` is a
trimmed core for YPIR and points there for the working version. Every bucket
shares one parameter set, so the client uploads one public-parameter set per
session. Parameters are spiral-rs's `CFG_20_256` lattice set (n = 2, p = 256,
q2 = 20 bits, t_gsw = 8, t_conv = 4, t_exp = 8/56, nu_1 = 9) with nu_2 sized to
the largest bucket; nu_2 is capped at 6, the validated value, and larger
buckets double `instances` instead. A Spiral item is one 8 KB plaintext
(`instances` x 8 KB), holding 256 x `instances` records.

The server path is upstream `process_query` with stage timers (expand,
first_dim, fold, pack, encode); the first bucket of the first trial asserts it
is byte-identical to upstream `process_query`. Every real record is decoded and
compared with the oracle outside the timers. Run single-threaded with
`RAYON_NUM_THREADS=1` on one pinned CPU (`scripts/run_spiral_matrix.py
--scheme spiral_pbc` does both).

Build with `RUSTFLAGS='-C target-cpu=native' cargo build --release`; the
AVX2 kernels are selected by the target feature, and a non-AVX2 fallback exists
for local correctness runs.

## TreePIR + Spiral

`treepir_spiral` (same crate) puts Spiral behind TreePIR's color buckets
instead: the TreePIR row's H colors of balanced size over the 2^(H+1) - 2
non-root nodes, filled with the SealPIR adapter's deterministic records, and
the same color-local indices and leaves, read from that row's `formal.json`
(whose rows carry the upstream SubCSA indices of each leaf's proof nodes). Its
rows therefore differ from the TreePIR row only in the per-color PIR backend.
Parameters and the timed `process_query` are the ones above, with nu_2 sized
to the largest color; every color shares one public-parameter set. Colors are
served one at a time (built, asked every path's query for that color, then
released), so one color database is resident (2 GiB at 4 GiB, 4 GiB at 8 GiB)
and a path's server time is the sum of its colors' process_query times.

The TreePIR repository also bundles the Spiral authors' C++ reference code
(`Spiral/Seperated`), which they mark as reference-only in favour of
spiral-rs; its `Answer.server` timer also leaves query expansion out. This
adapter therefore uses spiral-rs, as Spiral + PBC does.

    target/release/treepir_spiral --input <treepir-backend-tuning>/<scale>/formal.json --output treepir_spiral.json

`scripts/run_spiral_matrix.py --scheme treepir_spiral --treepir-root
<treepir-backend-tuning>` runs the heights.
