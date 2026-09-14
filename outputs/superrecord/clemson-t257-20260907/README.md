# Clemson Tree g32 t257 confirmation

Run the user-selected c8/t257, 32-byte entry, G32 construction against the
original c12/t4099 baseline at 1 GiB, 100 measurements plus 3 warmups each.
Accept actual full-response decryption correctness; record noise without a
minimum-bit threshold. Both configurations use source 932a4ed and identical
ring/ciphertext/response moduli, layout, and query leaf schedule.

Host: zcy05331@clnode239.clemson.cloudlab.us, Xeon Gold 6142, two NUMA nodes.
Bind CPU 0 and interleave memory across nodes, one thread, as in prior Clemson
runs. Reuse installed Intel HEXL; build two isolated binaries. The raw JSON
includes per-stage time and communication, labeled modeled query/keys versus
actual response. This is a single sequential baseline/candidate pair, not a
reverse-order repetition or a reused historical speed measurement.

Run root: /users/zcy05331/tree-t257-clemson-20260907
Launcher: launch.sh (uploaded under the run root).
Current status and logs are stored under metadata/; raw results under outputs/.
The full 48/64/128B Generic follow-up is a separate scope selection.
