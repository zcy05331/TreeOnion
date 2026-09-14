# Generic Entry 48B Sweep

Entry bits: 384. Leaves: 16777216 (2^24). Trials: 100; warmups: 3.
Logical DB bytes: 1610612736. Actual full tree bytes: 1610612688.
Logical DB throughput uses actual full-tree bytes divided by mean server time. It is not network throughput or padded-storage throughput.

| category | candidate | layout | mean ms | logical DB MiB/s | median ms | p95 ms | SD ms | query modeled B | response actual B | once keys modeled B | stage mean ms |
| --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- | --- |
| best h3 | generic-h3-mandatory | h=3 G=32 P=3 N0=352 b=8 hq=9 | 2952.264 | 520.279 | 2952.191 | 2959.761 | 4.621 | 14880 | 22528 | 1607040 | complete=70.028ms, expand=135.600ms, finalize=438.052ms, fold=441.297ms, pack=0.560ms, prepare=74.776ms, project=95.020ms, rotate=119.357ms, scan=1575.799ms, switch=1.048ms |
| best h1 | generic-h1-mandatory-0 | h=1 G=32 P=1 N0=512 b=9 hq=10 | 2580.088 | 595.328 | 2580.248 | 2587.184 | 5.059 | 14880 | 11264 | 1607040 | complete=73.014ms, expand=182.163ms, finalize=339.730ms, fold=298.185ms, pack=0.189ms, prepare=111.886ms, project=33.497ms, rotate=41.654ms, scan=1498.238ms, switch=0.611ms |

