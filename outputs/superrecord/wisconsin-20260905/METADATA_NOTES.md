# Metadata interpretation

The `tree_g32.json` field `metadata.hardware_target` is a legacy hardcoded
string naming CloudLab Xeon 6142. It is not runtime hardware discovery.
The Wisconsin run was executed on Xeon Silver 4314, verified by SSH `lscpu`
and the recorded environment. Preserve raw files; use the run environment
record for CPU identity. This metadata string does not change the algorithm
or the timing operation.

Current comparison scope: Layerwise's original-versus-tuned estimate is a sum
of per-layer profiling means. The new tuned full-path 100-query mean is a
separate measurement and must not be substituted into a paired comparison.
