# Experiment archive

This archive contains all experiment data available locally or retrieved from
Clemson as of September 8, 2026 UTC. It includes raw measured samples, stage
profiles, communication sizes, process memory/exit records, parameter searches,
correctness checks, failed/interrupted runs, figures and source snapshots.
Timing and communication definitions remain in the original files.

## Main collections

| Collection | Contents and completion boundaries |
|---|---|
| [Wisconsin 1/4 GiB](superrecord/wisconsin-20260905/README.md) | Completed seven-scheme matrix, ordinary OnionPIR reference, full TreePIR tuning and stage analysis. |
| [Clemson 1/8 GiB](superrecord/cloudlab-20260904/profiling/README.md) | Original and tuned baselines. The 8 GiB Generic run failed; independent TreePIR and Flat runs completed. |
| [Full Clemson TreePIR tuning](superrecord/cloudlab-20260904/treepir-backend-tuning/) | Failed candidates plus completed formal VBPIR fd256 1 GiB (16 measurements) and SealPIR d2 8 GiB (100 measurements). |
| [Entry-size summary](superrecord/clemson-entry-recovery-20260907/RESULTS.md) | Completed 16/48/64/128 B results, six candidates with 100 measurements each. 48 B uses a 1.5 GiB tree; other sizes use 1 GiB. |
| [Original Clemson entry runs](superrecord/clemson-entry-original-20260907/) | 16/64 B passed the old 1-bit policy; 32 B reuses its recorded earlier source; 128 B failed that policy. |
| [Recovered 48/128 B](superrecord/clemson-entry-recovery-20260907/) | Completed under decryption-only admission. These are separately identified from the original policy/version. |
| [Clemson t4099/t257](superrecord/clemson-t257-20260907/outputs/formal-1gb/REPORT.md) | Completed 1 GiB matched g32 comparison, 100 measurements each; equal communication and essentially equal measured speed. |
| [Local t4099/t257](superrecord/tree-t257-local-20260906/REPORT.md) | Rosetta 256 MiB comparison, both execution orders, 200 measurements per configuration. |
| [Decryption-only transition](superrecord/decryption-only-20260906/README.md) | Rule-change tests, original failures, authorized interruption and revised Wisconsin launch records. |
| [Noise diagnostics](superrecord/noise-diagnostic-cached-20260906/FINDINGS.md) | Completed six-shape, three-query diagnostic with original data and findings; not formal performance samples. |
| [Earlier interrupted diagnostics](superrecord/noise-diagnostic-20260906/) | Partial attempt preserved as interrupted, not a completed result. |
| [NUMA, preflight and historical revisions](superrecord/cloudlab-20260904/remote-outputs/) | Additional server outputs; associated [metadata](superrecord/cloudlab-20260904/remote-metadata/) includes perf samples and logs. |
| [Source snapshots](superrecord/cloudlab-20260904/source-snapshots/) | Original deployed source packages and launch records. |
| [Local validation and prototype runs](generic-superrecord-validation/) | Correctness, planning, profile and TreePIR validation artifacts. Other local prototype scales are under [superrecord](superrecord/). |
| [Historical node0/perf](perf/) | Earlier baseline data and machine metadata. |
| [Historical Merkle results](merkle_baselines/) | Includes eight additional artifacts copied from the main checkout without changing that checkout. |

## Interpretation

- Prefer the linked latest completed raw results over older dated summary
  snapshots. A directory containing logs does not imply successful completion.
- Status/PID files are preserved historical snapshots, not live queue state.
- Correctness-only admission replaced the 1-bit headroom gate during this
  campaign. Noise remains recorded; old results retain their original policy.
- Generic/g32 return full ancestor paths including the root. Other legacy path
  baselines exclude the root; the ordinary OnionPIR reference returns a block.
- Modeled seeded queries/evaluation keys and actual response serialization are
  labeled separately; TreePIR uses its documented native serialization.
- Calibration, smoke, partial and failed runs are not added to formal counts.

## Known unavailable data

The original Wisconsin node expired, confirmed by the user. Only the revised
16 B run was retrieved as a completed formal result. Final Wisconsin
48/64/128 B and the queued Wisconsin t257 outcomes were not retrieved; their
completion is unknown. Clemson follow-ups are separate measurements and do
not recreate those Wisconsin raw samples. See [the access record](superrecord/decryption-only-20260906/ACCESS_STATUS.md).

## Inventory and exclusions

[archive-inventory.json](archive-inventory.json) lists archived payload files,
byte sizes and collection totals. JSON and gzip/tar readability were checked.
Build executables, object/dependency files, dependency installations and Python
caches are excluded; source fixtures, source packages and actual perf capture
data are retained. Original source-code commits remain in the branch history.
