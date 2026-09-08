# Clemson entry-size results

All four requested entry sizes have completed results. Each has six candidates and 100 measured queries per candidate, plus three warmups. Select the minimum mean server time within each h group.

| Entry B | Nominal DB GiB | h3 ms | h1 ms | h3 MiB/s | h1 MiB/s | h3 response B | h1 response B |
|---|---:|---:|---:|---:|---:|---:|---:|
| 16 | 1 | 2050.704 | 2323.625 | 499.341 | 440.691 | 11264 | 11264 |
| 48 | 1.5 | 2952.264 | 2580.088 | 520.279 | 595.328 | 22528 | 11264 |
| 64 | 1 | 1985.749 | 2211.638 | 515.674 | 463.005 | 22528 | 11264 |
| 128 | 1 | 1970.701 | 2143.730 | 519.612 | 477.672 | 33792 | 22528 |

Queries are 14880 B modeled seeded payloads; response sizes are actual ciphertext payload serialization. Throughput uses actual full-tree bytes, including the 48B row at about 1.5 GiB.

16B/64B are the completed original c5eb5ae runs with a 1-bit gate. 48B/128B are source 932a4ed recovery runs with decryption-only admission; margins remain diagnostic. They are not a single uniform-policy run. The original 128B failure remains separately archived.

All corresponding raw JSON, process exit records, per-entry analysis and logs were copied to this local repository.
