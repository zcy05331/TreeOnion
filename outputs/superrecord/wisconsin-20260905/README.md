# Wisconsin complete experiment archive

Host: sm110p-10s10611.wisc.cloudlab.us, Xeon Silver 4314, one NUMA node.
Completed logical database tiers: 1 GiB and 4 GiB, 32-byte entries.
Core methods: Generic h3/h1, tuned Layerwise, g32, tuned Cuckoo, TreePIR, Flat.
The Standard single-block reference is also included and is not a path query.
Most formal rows use 100 measurements plus 3 warmups; Cuckoo, TreePIR and Flat
use 16 measurements plus 3 warmups.

- [Stage analysis and explanation](analysis/REPORT.md)
- [Stage CSV](analysis/stages.csv)
- [Generic shape/counter CSV](analysis/generic-shapes.csv)
- [h3 savings and penalties](analysis/h3-savings.csv)
- [Paired bootstrap results](analysis/analysis.json)
- [Raw formal and original layout-tuning data](profiling/)
- [Full TreePIR backend tuning records](treepir-backend-tuning/)
- [Build, validation and run metadata](remote-metadata/)

Onion measurements use the frozen core snapshot 10a172d; the crypto C++ is
unchanged from 278e4dc. TreePIR adapters were staged from 1d0c732. Later local
code/script commits do not retroactively change the measured binary identity.
The timing source, complete per-query samples, communication labels and process
exit/peak-memory records remain in the raw JSON and metadata.

From the repository root, regenerate numeric analysis and PDF figures:

```bash
python3 scripts/analyze_wisconsin_profiles.py \
  --root outputs/superrecord/wisconsin-20260905/profiling \
  --output outputs/superrecord/wisconsin-20260905/analysis
Rscript scripts/plot_wisconsin_profiles.R \
  outputs/superrecord/wisconsin-20260905/analysis
```

Numeric analysis uses only the Python standard library and existing project
analysis helpers. PDF plotting uses base R. The included PNG previews were
rendered from those PDFs. No C++ instrumentation was changed for this report.
