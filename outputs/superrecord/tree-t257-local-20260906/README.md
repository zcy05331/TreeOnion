# Tree g32 t=257 comparison

The user selected 8-bit coefficient payloads with plaintext modulus 257,
32-byte entries, G=32. Both builds share n=2048, the composite 29+29-bit
ciphertext modulus, 22-bit response modulus and gadget lengths.

Run locally before uploading or scheduling on Wisconsin. The local host has
16 GiB RAM and runs x86_64 Intel HEXL through Rosetta. Use 2^22 leaves
(256 MiB nominal full tree), with 100 measurements plus 3 warmups per build.
The full-tree payload is (2*2^22-1)*32 = 268435424 bytes.
Local results compare two translated builds and are not Wisconsin estimates.

Before timing, run the HEXL kernel witness, tree_g32 correctness gate, and
a three-query small benchmark for both configurations. Every benchmark
query also validates all response coefficients, including unused slots,
and requires at least 1 bit of measured noise margin outside the timers.

The same source snapshot is compiled as CONFIG_N2048_K1_COMP (t=4099) and
CONFIG_N2048_K1_COMP_T257 (t=257). Results must report matching public
layouts and query leaf schedules, stage times, communication and noise.

Once local validation and timing succeed, upload the source and queue the
1 GiB (2^24 leaves) 100+3 comparison after Wisconsin entry-size experiments
finish. Build and run only after that predecessor exits; retain any failed
predecessor status separately.
