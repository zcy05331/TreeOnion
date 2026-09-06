# Clemson profiling archive

This directory preserves the locally synchronized experiment files from the
September 4 campaign. Logs, status rows and generated summaries are historical
snapshots, not a live view of the server. The consolidated completed-result
comparison is in [the throughput report](../../throughput-20260906/REPORT.md).

The 8 GiB Generic run exited with code 1 after a noise-margin rejection and
produced no formal samples. Its final process record and log are included;
the scheduler's last `running` row predates its collection of that failure.
See [the failure investigation](../failures/8gb-generic-20260906/INVESTIGATION.md)
and [the fresh noise diagnostic](../../noise-diagnostic-cached-20260906/FINDINGS.md).

The 1 GiB TreePIR result here is the original formal run. Later tuning pilots
are archived separately under [treepir-tuning](../treepir-tuning/); they do not
replace formal results. The unfinished queue is not represented as complete.
