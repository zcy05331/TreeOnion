# NTT-domain Generic alpha pyramid

Apply the supplied optimization to the shared Generic composite-m32 alpha
preparation path and profile h=3. Transform only the bottom N0 ciphertexts,
then perform the original pairwise sums and unpaired carries in NTT form.
Keep the coefficient-domain reference/noise oracle and legacy g32 unchanged.

For N0=352, keep the same 705 pyramid ciphertexts, 351 additions and two
carries, but transform only 352 ciphertexts. The m32 split still processes
all rows. Update alpha_ntt_ciphertexts in the composite cost model so tuning
does not keep charging for removed transforms. Per-add/copy operations and
per-ciphertext NTT/split calibration units stay unchanged.

Correctness: compare every NTT c0/c1 limb and both m32 buffers with the original
pipeline, including odd widths, modulus-boundary values and source immutability.
Run the existing end-to-end and noise tests. No noise-headroom rule changes.

Performance: alternate old/new preparation on identical inputs for 100 samples,
including m32 splitting. Also compare original/new binaries at 256 MiB locally
with h=3, G=64, P=1, N0=352, b=7, 100 measurements and 3 warmups. Keep expansion
height 9 and alpha alignment 16; verify the resulting shape matches before
claiming a direct speedup. This local Rosetta test is not a 1 GiB server result.
