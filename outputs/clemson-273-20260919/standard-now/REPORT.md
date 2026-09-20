# Standard OnionPIRv2: new Clemson1GiB reference

Native Xeon Platinum8360Y;CPU0,memorypreferredNUMA0. Source `4e823822d9c0f2f3571f187b4119e21869716061`;defaultEP6,HEXL1.2.6 Release.50measuredqueries after3warmups,seed7;allcorrect.

| Metric | Result |
|---|---:|
| Server mean | 906.011ms |
| Sample SD | 3.860ms |
| Original logical DB throughput | 1130.230MiB/s |
| Query (modeled seeded) | 14880B |
| Response ciphertext payload (actual) | 11264B |
| Setup | 11.826s |
| PeakprocessRSS | 5.444GiB |

Standard retrieves one3KiB plaintext block. The logical database is1GiB minus64B due to the existing root-excluded tree source; it is not a complete-path response. Native57/57 correctness regressions passed before measurement.

Before launch, other-project processes 15294 and 20293 were stopped (T), with unchanged CPU ticks in two samples. The pre-measurement idle gate passed. A later follow-up after benchmark completion observed them running again; their resumption time was not continuously recorded. This job neither resumed nor terminated them.

| Exclusive server stage | Mean ms |
|---|---:|
| convert | 21.375155 |
| expand | 109.660271 |
| first_dim_core | 549.268600 |
| first_dim_finalize | 54.027916 |
| first_dim_query_ntt | 7.780714 |
| first_dim_query_pack | 21.017054 |
| mod_switch | 0.116964 |
| other_dim | 141.054692 |
| other | 1.709148 |

RawJSON and processrecords are under `native/outputs/1gb/`;hardware/compiler/build/idle-check evidence is under `native/metadata/`. Optional environment strings in rawJSON were not populated by the standalone invocation; provenance is recorded in this summary and the sidecars. Rawmeasurements were not edited.

This Standard baseline was explicitly authorized to run immediately. The rest of the1/4GiBmatrix remains scheduled for09/20 03:00America/Chicago and must wait for serveridle. Reuse this validated Standard row rather than rerunning it merely for scheduling.
