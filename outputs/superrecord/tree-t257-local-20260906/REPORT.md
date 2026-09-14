# Local Tree g32 t257 result

Apple Silicon with x86_64 Rosetta; 256 MiB full tree; 32-byte entries. Each order uses 100 measurements and 3 warmups per configuration. All 400 measured queries passed full-response correctness and the 1-bit noise gate.

| Order | t4099 ms | t257 ms | t257 throughput gain |
|---|---:|---:|---:|
| 4099 then 257 | 795.114 | 698.209 | 13.88% |
| 257 then 4099 | 757.413 | 723.157 | 4.74% |
| Both orders combined | 776.263 | 710.683 | 9.23% |

Combined throughput: 329.785 vs 360.217 MiB/s.

Both have modeled query 14880 B, actual response 11264 B and modeled helper keys 1607040 B. Minimum measured noise margins are 1 bit (t4099) and 5 bits (t257).

The measured gain varies with run order; the first 13.9% result is not a stable server-speedup claim. Public layouts and scan counts are unchanged. Wisconsin confirmation is queued only after this local comparison passes, and after the entry-size predecessor exits.

- [Forward-order raw results and stage table](formal-256mib/REPORT.md)
- [Reverse-order raw results](reversed-256mib/)
- [Protocol and scope](README.md)
