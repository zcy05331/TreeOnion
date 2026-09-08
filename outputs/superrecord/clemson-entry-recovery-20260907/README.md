# Clemson missing entry-size recovery

Run 48B and 128B Generic h1/h3 with 100 measurements plus 3 warmups per
candidate, using decryption-only admission and diagnostic noise records.
48B retains the 32B reference leaf count (2^24), giving 1.5 GiB full-tree
capacity; 128B uses 2^22 leaves, giving 1 GiB.

Reuse the n2048 composite baseline binary from the completed t257 comparison,
source 932a4ed. CPU0, interleave all NUMA nodes, one thread, planner budget
80 GiB. Raw profiles retain all stages and communication accounting.

The earlier Clemson 16B and 64B runs completed under source c5eb5ae and the
old 1-bit policy; archive them separately rather than relabeling them as new
runs. The old 128B run failed the noise-headroom gate and is not a complete
performance point. The Wisconsin missing final outcomes cannot be inferred
from these Clemson results.
