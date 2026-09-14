# Generic Entry 16B Sweep

Entry bits: 128. Leaves: 33554432 (2^25). Trials: 100; warmups: 3.
Logical DB bytes: 1073741824. Actual full tree bytes: 1073741808.
Logical DB throughput uses actual full-tree bytes divided by mean server time. It is not network throughput or padded-storage throughput.

| category | candidate | layout | mean ms | logical DB MiB/s | median ms | p95 ms | SD ms | query modeled B | response actual B | once keys modeled B | stage mean ms |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| best h3 | generic-h3-mandatory | h=3 G=32 P=1 N0=352 b=9 hq=9 | 1000.708 | 1023.275 | 1001.635 | 1002.720 | 5.224 | 14880 | 11264 | 1607040 | complete=40.135ms, expand=66.805ms, finalize=81.509ms, fold=165.845ms, pack=0.168ms, prepare=33.485ms, project=16.518ms, rotate=23.322ms, scan=572.540ms, switch=0.096ms |
| best h1 | generic-h1-mandatory-0 | h=1 G=4 P=3 N0=512 b=7 hq=10 | 1163.791 | 879.883 | 1163.713 | 1164.918 | 0.708 | 14880 | 11264 | 1607040 | complete=42.851ms, expand=89.753ms, finalize=49.478ms, fold=121.839ms, pack=0.506ms, prepare=50.594ms, project=71.637ms, rotate=99.800ms, scan=636.828ms, switch=0.096ms |

