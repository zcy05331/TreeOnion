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
decoder boundary=508.6282 and1-bit boundary=254.3141. Correct selected entries
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
