# Generic Entry 32B Sweep

Entry bits: 256. Leaves: 16777216 (2^24). Trials: 100; warmups: 3.
Logical DB bytes: 1073741824. Actual full tree bytes: 1073741792.
Logical DB throughput uses actual full-tree bytes divided by mean server time. It is not network throughput or padded-storage throughput.

| category | candidate | layout | mean ms | logical DB MiB/s | median ms | p95 ms | SD ms | query modeled B | response actual B | once keys modeled B | stage mean ms |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| best h3 | bundle3 | h=3 G=64 P=1 N0=352 b=9 hq=9 | 2010.239 | 509.392 | 2010.257 | 2015.120 | 2.678 | 14880 | 11264 | 1607040 | complete=69.921ms, expand=142.764ms, finalize=322.937ms, fold=296.587ms, pack=0.189ms, prepare=75.417ms, project=28.873ms, rotate=33.774ms, scan=1038.458ms, switch=0.612ms |
| best h1 | generic-h1-mandatory-1 | h=1 G=8 P=3 N0=512 b=7 hq=10 | 2280.484 | 449.027 | 2280.510 | 2285.160 | 2.864 | 14880 | 11264 | 1607040 | complete=73.973ms, expand=191.923ms, finalize=201.371ms, fold=217.796ms, pack=0.549ms, prepare=111.726ms, project=132.933ms, rotate=156.765ms, scan=1191.844ms, switch=0.611ms |

Reused 32B source: `/users/zcy05331/generic-superrecord-bench-20260904/outputs/profiling/1gb/generic.json` at source `22adeee`. Preprocessing time belongs to this original run; compare setup costs across entry sizes only when the implementation and execution conditions match. This reference predates the bytewise preprocessing optimization used by the new runs.
