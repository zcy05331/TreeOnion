# Generic Entry 128B Sweep

Entry bits: 1024. Leaves: 4194304 (2^22). Trials: 100; warmups: 3.
Logical DB bytes: 1073741824. Actual full tree bytes: 1073741696.
Logical DB throughput uses actual full-tree bytes divided by mean server time. It is not network throughput or padded-storage throughput.

| category | candidate | layout | mean ms | logical DB MiB/s | median ms | p95 ms | SD ms | query modeled B | response actual B | once keys modeled B | stage mean ms |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| best h3 | generic-h3-mandatory | h=3 G=256 P=1 N0=352 b=9 hq=9 | 1970.701 | 519.612 | 1970.291 | 1978.261 | 3.905 | 14880 | 33792 | 1607040 | complete=64.121ms, expand=131.586ms, finalize=327.577ms, fold=297.269ms, pack=0.174ms, prepare=74.619ms, project=15.695ms, rotate=19.763ms, scan=1037.769ms, switch=1.458ms |
| best h1 | generic-h1-mandatory-0 | h=1 G=32 P=3 N0=512 b=7 hq=10 | 2143.730 | 477.672 | 2143.993 | 2147.611 | 2.825 | 14880 | 22528 | 1607040 | complete=67.017ms, expand=178.804ms, finalize=193.088ms, fold=216.082ms, pack=0.505ms, prepare=112.037ms, project=89.668ms, rotate=112.028ms, scan=1172.478ms, switch=1.047ms |

