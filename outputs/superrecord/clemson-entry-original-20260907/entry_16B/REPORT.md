# Generic Entry 16B Sweep

Entry bits: 128. Leaves: 33554432 (2^25). Trials: 100; warmups: 3.
Logical DB bytes: 1073741824. Actual full tree bytes: 1073741808.
Logical DB throughput uses actual full-tree bytes divided by mean server time. It is not network throughput or padded-storage throughput.

| category | candidate | layout | mean ms | logical DB MiB/s | median ms | p95 ms | SD ms | query modeled B | response actual B | once keys modeled B | stage mean ms |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| best h3 | generic-h3-mandatory | h=3 G=32 P=1 N0=352 b=9 hq=9 | 2050.704 | 499.341 | 2050.575 | 2056.065 | 3.216 | 14880 | 11264 | 1607040 | complete=73.681ms, expand=144.696ms, finalize=345.376ms, fold=296.100ms, pack=0.304ms, prepare=74.990ms, project=34.868ms, rotate=42.004ms, scan=1037.362ms, switch=0.611ms |
| best h1 | generic-h1-mandatory-0 | h=1 G=4 P=3 N0=512 b=7 hq=10 | 2323.625 | 440.691 | 2323.664 | 2326.905 | 1.768 | 14880 | 11264 | 1607040 | complete=76.649ms, expand=194.612ms, finalize=198.518ms, fold=218.822ms, pack=0.923ms, prepare=112.877ms, project=150.980ms, rotate=184.943ms, scan=1183.675ms, switch=0.612ms |

