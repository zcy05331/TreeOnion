# Generic Entry 64B Sweep

Entry bits: 512. Leaves: 8388608 (2^23). Trials: 100; warmups: 3.
Logical DB bytes: 1073741824. Actual full tree bytes: 1073741760.
Logical DB throughput uses actual full-tree bytes divided by mean server time. It is not network throughput or padded-storage throughput.

| category | candidate | layout | mean ms | logical DB MiB/s | median ms | p95 ms | SD ms | query modeled B | response actual B | once keys modeled B | stage mean ms |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| best h3 | generic-h3-mandatory | h=3 G=128 P=1 N0=352 b=9 hq=9 | 1985.749 | 515.674 | 1985.440 | 1990.450 | 2.845 | 14880 | 22528 | 1607040 | complete=67.265ms, expand=136.303ms, finalize=319.802ms, fold=297.783ms, pack=0.281ms, prepare=75.125ms, project=21.552ms, rotate=27.048ms, scan=1038.847ms, switch=1.045ms |
| best h1 | generic-h1-mandatory-0 | h=1 G=16 P=3 N0=512 b=7 hq=10 | 2211.638 | 463.005 | 2211.871 | 2214.723 | 1.894 | 14880 | 11264 | 1607040 | complete=70.666ms, expand=184.147ms, finalize=195.993ms, fold=215.502ms, pack=0.838ms, prepare=111.873ms, project=107.929ms, rotate=135.133ms, scan=1187.949ms, switch=0.610ms |

