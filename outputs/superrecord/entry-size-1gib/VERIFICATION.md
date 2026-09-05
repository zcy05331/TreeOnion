# Entry-size runner verification

- Local C++ regression: 39/39 applicable tests passed.
- CLI regression: passed.
- Python tests: 28 passed, including actual shell routing with temporary test doubles.
- Encrypted entry checks: 16/32/64/128 bytes, each with 4096 leaves, 1 warmup and 2 samples per candidate; six h1/h3 candidates per entry.
- Bounded independent review: no control-flow or path defect found in predecessor waiting, failure propagation and reference reuse.

These are local validation results. They are not the new 1 GiB, 100-sample native measurements. The queued native job repeats regression and encrypted checks before measurement.
