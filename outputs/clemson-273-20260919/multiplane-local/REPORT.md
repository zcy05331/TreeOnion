# Old-layout h=1 multi-plane local verification

The primary algorithm now uses the original gamma/alpha/beta layout with h=1,
variable power-of-two g, and P=ceil(entry_bits/(floor(log2(t))*g)).
Every plane uses node=p+u*R_level. The query and NTT-first alpha preparation
are shared; per-plane selection/rotation feeds Projection or MLWE response
packing. Existing Tree g32 and Generic entry points retain their behavior.

## Validation

- CONFIG_N2048_K1_COMP default EP6: 58/58 applicable C++ tests passed.
- Full scripts/run_regression.sh completed with `regression OK`, including
  existing CLI contracts, 24 binary-backed Python tests, and the new g8/g16/g32
  old-layout benchmark chain. See regression-default-final.log.
- Sweep-script unit tests: 6/6 passed (python-sweep-tests.log).
- EP3/drop32, exact KS8: new encrypted multiplane test passed, including
  full response plaintext/zero residues across response-chunk boundaries.
- EP3 benchmark-contract smokes: 14 configurations, 34 measured queries plus
  14 warmups, all correctly decrypted. Entry sizes 16/32/48/64/128 bytes;
  g8/g16/g32; P1/P2/P3; Projection/MLWE; interleaved/split-column preparation.
  Exact shapes and noise diagnostics are in smoke-summary.json. Each case
  retains its raw JSON, process exit, peak memory, and log.
- Independent read-only review found no actionable issues in the new core,
  benchmark integration, and ranking script.

## Evidence boundary

These are small local correctness and report-contract runs on macOS under
x86_64/Rosetta, not native 1/4 GiB throughput results. They establish neither
an optimal large-database g/N0 nor a speedup over historical Generic. No new
source or experiment was submitted to the remote server during this change.
The earlier regression-driver-interrupted.log contains a shell-reader error
because the regression script was edited while that initial invocation ran;
the complete stable-script rerun is regression-default-final.log.

The full EP3 legacy suite was not rerun: this parameter point has historical
legacy-test assumptions tied to EP6. The new EP3 pipeline and both response
modes were directly verified. The main checkout and original baselines were
kept separate from this worktree.
