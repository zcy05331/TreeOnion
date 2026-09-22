# Scan-buffer reuse A/B

Old campaign ep3 binary against the rebuilt one carrying the ScanScratch change, alternating processes on an idle host, identical settings (g8, N0 512, split-columns, seed 7, 50 measured queries, 3 warmups).

| Run | scan_core ms | sd | server total ms | correct |
|---|---:|---:|---:|---|
| 1gib-new-r1 | 708.0 | 1.3 | 973.1 | True |
| 1gib-new-r2 | 702.4 | 0.5 | 967.1 | True |
| 1gib-old-r1 | 712.0 | 0.7 | 971.9 | True |
| 1gib-old-r2 | 713.3 | 1.1 | 973.6 | True |
| 4gib-new-r1 | 2750.0 | 15.0 | 3338.7 | True |
| 4gib-old-r1 | 2874.6 | 105.3 | 3485.1 | True |

The 4 GiB old-binary run was disturbed between trials 17 and 26 (scan core rose from about 2828 to about 3130 ms and returned). Comparing the undisturbed segments gives 2828 to 2759 and 2796 to 2733 ms, about 2.4 percent. Peak resident memory and correctness are unchanged.
