# Clemson 8 GiB Generic noise-margin failure investigation

Observed: exit1 after12489.4958s (3h28m09s), peakRSS318861582336B.
The failure log has no completed warmup or measured block, and no final
Generic JSON. Other completed baseline outputs are preserved.

The failing assertion follows path-size and every selected-entry equality
checks. Thus the selected path of that invocation decrypted correctly, but
the minimum small-q residual margin did not meet the configured1-bit floor.
The first warmup leaf from the preserved seed7/100-trial schedule is15830119;
subsequent warmup leaves are113195322 and40120694.

The full run prepares candidates first, then shuffles their first warmup order.
The final preparing log mentions frontier-2, but this does not identify the
failing candidate. Old logs omitted its label and actual residual, and the
exited process did not preserve its client key/query randomness for exact replay.

Budget calculation (src/client.cpp) is integer log2(q_small/(2*t*max_residual)),
where residual is measured against the decoded coefficient over the entire
response polynomial. For this fixed config: q_small=4169729, t=4099,
decoder boundary=508.6276 and1-bit boundary=254.3138. Correct selected entries
and a budget below1 can therefore coexist.

Target preflight uses a finite set of boundary leaves/patterns and fresh
queries. It does not certify every later fresh response. Its selected leaves
include the first measured leaf, but omit the first warmup leaf15830119.
This explains how preflight acceptance and runtime margin rejection can coexist;
it does not yet identify the failing shape or noise-growth stage.

Next diagnostic: stream the original six prepared layouts level-by-level with
actual benchmark bytes and the three original warmup leaves; inspect optimized
m32 noise before/after projection, pooling and modulus switch. These are
diagnostic observations, not formal performance samples or a forged noise-gate
certificate. The threshold remains1. Future benchmark failure messages now
include candidate, phase/block, leaf, layout, budget and maximum residual.

## Diagnostic launch

Revision247a8c6 was deployed under
`/users/zcy05331/generic-superrecord-bench-20260904/noise-diagnostic-20260906`,
in tmux `generic-noise-diagnostic`. Native build and small-H smoke succeeded.
Six fixed shapes are being checked, three original warmup leaves each, with
fresh key/query randomness per isolated shape. Original parent18736 remains
held and the failed output remains untouched.

Independent source review confirmed the streaming path uses the same m32
packing, selection, rotation, projection and pooling primitives as the formal
path. Raw intermediate projection budgets are explicitly labeled raw and are
not treated as normalized preflight budgets. The final post-switch threshold
is the pass/fail criterion.

## Confirmed fixture coverage weakness

`fixture-coverage.json` evaluates the same source-pattern and pairing formulas.
For all six shapes, structured patterns0/2/3/4 repeat with periodh*rho nodes,
which is exactly one full plaintext block. Equal full plaintext blocks produce
equal first-dimension ciphertext rows; a CMux of equal rows has zero difference
and does not exercise its data-dependent selector-error term. Tail/padding
rows can still differ, so this is not a claim that every fold is noise-free.

Pattern1 is the actual deterministic random-looking benchmark database. The
h1/G8/P3 shape sees that source only atleaf1; the other five shapes see it at
leaf1 or2 andthe final leaf. The first scheduled warmup leaf15830119 is absent.
This is a concrete coverage gap, not proof of which layout caused the historical
exception. Fresh ciphertext randomness is another difference between those
preflight fixtures andthe later warmup.

The first diagnostic attempt was interrupted for redundant preprocessing before
using any outcome to select a retry. Revision8af612c caches one candidate's
M32 data across three queries and runs in a separate preserved directory:
`/users/zcy05331/generic-superrecord-bench-20260904/noise-diagnostic-cached-20260906`.
The cryptographic operations, original six layouts, three leaves andthreshold1
are unchanged. Results remain diagnostic only.
