# Height-11 encrypted roundtrip

The local probe passed eight encrypted queries and 768 node comparisons.
It checks N0=512 and N0=1024 on the same 4096-plaintext source under one
session with eleven expansion keys. Query points are 0, 1023, 1024 and
4095; every selected plaintext's 96 nodes is compared with its source
coordinates.

This is a small correctness check on local x86_64/Rosetta. It does not
measure native performance, validate 1/8 GiB noise behavior, or exercise
the response wire codec. The height-11 checks observed one remaining
budget bit; the height-10 checks observed two. Fresh sessions may produce
different noise values.

To replay from the generic-superrecord worktree, use its existing
CONFIG_N2048_K1_COMP x86_64 build and local HEXL installation:

```sh
cmake --build build-x86_64-benchmark -j 4
python3 outputs/superrecord/cloudlab-20260904/parameter-alignment/h11-roundtrip/run.py
```

The runner links against the freshly built core objects and writes
`results.json` and `run.log`. No server access is involved.
