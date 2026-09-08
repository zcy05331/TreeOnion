# Clemson profiling archive

This directory preserves synchronized experiment results from the September 4
campaign, including later completed runs. Logs and status rows are historical
snapshots rather than a live view of the server.

The 8 GiB Generic run exited with code 1 after the old noise-margin rejection
and produced no complete formal result. Its final process record and log are
included. See [the failure investigation](../failures/8gb-generic-20260906/INVESTIGATION.md)
and [the fresh noise diagnostic](../../noise-diagnostic-cached-20260906/FINDINGS.md).

The independent 8 GiB TreePIR (100 measurements) and Flat (16 measurements)
runs subsequently completed. The matrix controller's final failed status
still records the Generic failure; it does not invalidate those completed rows.

The 1 GiB treepir.json file is the original formal reference. The later
retuned VBPIR fd256 1 GiB result has 16 measurements and is also preserved as
treepir_tuned.json. The [full backend-tuning archive](../treepir-backend-tuning/)
contains candidate failures and both formal tiers. The older
[throughput snapshot](../../throughput-20260906/REPORT.md) predates some of these
final files and must be read with its recorded timestamp.
