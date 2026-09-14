# Reproduced small-q margin rejection

Fresh diagnostic replication, source8af612c; not formal performance data.
Original H27/E256 h1/G8/P3/N0=512/b10/hq10, registered key height11.
The three original warmup leaves were used without changing threshold1.

| leaf | full response plaintext correct | maximum small-q residual | final budget bits |
|---:|---|---:|---:|
| 15830119 | yes | 174 | 1 |
| 113195322 | yes | 158 | 1 |
| 40120694 | yes | 256 | 0 |

For the failing fresh query, the largest residual is on allocated response
support (256); the maximum outside that support is101. This is not merely a
large residual in an unused polynomial position. Every coefficient equals its
expected plaintext, including packed neighbors and zero padding.

The q_small=4169729, t=4099 decoder-bound metric is508.627592. One-bit margin
requires maximum residual<=254.313796. Residual256 gives about0.990 bit,
which the integer budget reports as0. The assertion therefore behaves as
specified even though the response still decrypts correctly.

The recorded full-q budget before the final switch is1 bit; after the switch
it is0. The response was already near the required margin before switching,
and quantization in the final switch crosses the acceptance boundary. The
first-dimension/folding output minimum is already1 bit, down from at least19
bits in expanded alpha. Intermediate raw projection budgets are not directly
comparable to normalized preflight budgets.

The historical process omitted its failed candidate identity and random
key/query state. This reproduces the same failure class for the h1 shape;
it does not prove that this exact shape or ciphertext caused the old run's
first-warmup exception. All six shapes have now completed their three prescribed queries. All18
returned correct plaintext;17 met the one-bit threshold. The only reproduced
shortfall was the h1/G8/P3/N0=512 query above. The three h3 layouts had maximum
residuals134 (G64/P1/N0=352),206 (G64/P1/N0=683),136 (G32/P2/N0=352), with
all9 diagnostic queries meeting the threshold. These three-query diagnostics
do not supply the missing100-query formal8GiB Generic measurement.
The preflight fixture coverage weakness is documented separately in
../cloudlab-20260904/failures/8gb-generic-20260906/INVESTIGATION.md.

The measured256 residual exceeds the one-bit limit by about0.66%; reported
0 bits means the integer budget is below1, not that no decoding tolerance
remains. Its unrounded margin is0.990466 bits. No noise threshold or
cryptographic parameter was relaxed during this investigation.
