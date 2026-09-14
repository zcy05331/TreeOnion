#!/usr/bin/env bash
# Same executable and CPU, changing only anonymous-memory placement.
set -euo pipefail
RUN_ROOT="${1:?usage: run_cloudlab_numa_ab.sh RUN_ROOT}"
BIN="$RUN_ROOT/binaries/Onion-PIR-8135955"
OUT="$RUN_ROOT/outputs/numa-ab"
mkdir -p "$OUT"
export OMP_NUM_THREADS=1 OPENBLAS_NUM_THREADS=1 MKL_NUM_THREADS=1
export ONIONPIR_BENCH_COMMIT=813595513515d8010a8a8b5fd21ba249325c6893
export ONIONPIR_BENCH_BRANCH=codex/generic-superrecord
export ONIONPIR_BENCH_CPU="$(lscpu | sed -n 's/^Model name:[[:space:]]*//p')"
export ONIONPIR_BENCH_OS="$(uname -sr)"
export ONIONPIR_BENCH_COMPILER="$(g++ --version | head -n1)"
export ONIONPIR_BENCH_CMAKE="$(cmake --version | head -n1)"
export ONIONPIR_BENCH_HEXL_PATH="$RUN_ROOT/deps/hexl-install/lib64/cmake/hexl-1.2.6"
export ONIONPIR_BENCH_PROCESS_ARCH=x86_64
numactl --hardware > "$OUT/numa-hardware.txt"
echo running > "$OUT/status.txt"
for LABEL in A1 B1 B2 A2; do
  if [[ "$LABEL" == A* ]]; then POLICY=(--interleave=all); else POLICY=(--membind=0); fi
  printf '%s\n' "$LABEL" > "$OUT/current-run.txt"
  python3 "$RUN_ROOT/source/scripts/read_process_peak_memory.py" \
    --log "$OUT/$LABEL.log" --output "$OUT/$LABEL-process.json" \
    -- numactl --physcpubind=0 "${POLICY[@]}" "$BIN" \
    --test merkle_benchmarks --benchmark-case merkle_layerwise \
    --leaf-count 16777216 --experiments 16 --warmup 3 --trial-seed 7 \
    --benchmark-json "$OUT/$LABEL.json" &
  RUNNER_PID=$!
  (
    sleep 5
    CHILD_PID="$(pgrep -P "$RUNNER_PID" | head -n1 || true)"
    if [[ -n "$CHILD_PID" ]]; then
      numastat -p "$CHILD_PID" > "$OUT/$LABEL-numastat.txt"
      sed -n '/Cpus_allowed_list/p' "/proc/$CHILD_PID/status" > "$OUT/$LABEL-affinity.txt"
    fi
  ) &
  OBSERVER_PID=$!
  if ! wait "$RUNNER_PID"; then
    wait "$OBSERVER_PID" || true
    echo failed > "$OUT/status.txt"
    exit 1
  fi
  wait "$OBSERVER_PID"
done
python3 - "$OUT" <<'PY'
import json, statistics, sys
from pathlib import Path

root = Path(sys.argv[1])
runs = {}
leaves = None
for label in ('A1', 'B1', 'B2', 'A2'):
    report = json.loads((root / f'{label}.json').read_text())
    case = report['cases'][0]
    assert case['name'] == 'merkle_layerwise' and case['correctness_passed']
    assert report['workload']['leaf_count'] == 16777216
    assert report['workload']['measured_trials'] == 16
    assert len(case['server_compute_samples_ms']) == 16
    if leaves is None:
        leaves = case['sample_leaf_indices']
    assert case['sample_leaf_indices'] == leaves, 'leaf schedules differ'
    runs[label] = case

def stats(values):
    return dict(count=len(values), mean_ms=statistics.fmean(values),
                median_ms=statistics.median(values), stdev_ms=statistics.stdev(values))

groups = {}
for name, labels in (('interleaved', ('A1', 'A2')), ('local_node0', ('B1', 'B2'))):
    cases = [runs[label] for label in labels]
    server = [x for case in cases for x in case['server_compute_samples_ms']]
    stages = {}
    for stage in cases[0]['pipeline_profile_ms']:
        stages[stage] = stats([sample['stages'][stage] for case in cases
                              for sample in case['pipeline_profile_samples_ms']])
    groups[name] = dict(server=stats(server), stages=stages,
                        individual_means_ms={label:statistics.fmean(runs[label]['server_compute_samples_ms'])
                                             for label in labels})
a = groups['interleaved']['server']['mean_ms']
b = groups['local_node0']['server']['mean_ms']
summary = dict(source_commit='813595513515d8010a8a8b5fd21ba249325c6893',
               cpu=0, run_order=['A1', 'B1', 'B2', 'A2'],
               warmups_per_process=3, measured_trials_per_process=16,
               same_leaf_schedule=True, groups=groups,
               interleaved_over_local_latency_ratio=a/b,
               local_latency_reduction_percent=100*(1-b/a))
(root / 'comparison.json').write_text(json.dumps(summary, indent=2) + '\n')
lines = ['# NUMA placement at fixed CPU 0', '',
         'The original measurement executable is used for all four runs, in A-B-B-A order.',
         'Each process performs three warmups and sixteen measured queries with the same leaf schedule.',
         '', '| Metric | Interleaved across nodes | Local node 0 |',
         '| --- | ---: | ---: |',
         f'| Server mean, ms | {a:.3f} | {b:.3f} |']
for stage in groups['interleaved']['stages']:
    left = groups['interleaved']['stages'][stage]['mean_ms']
    right = groups['local_node0']['stages'][stage]['mean_ms']
    lines.append(f'| {stage}, ms | {left:.3f} | {right:.3f} |')
lines += ['', f'Local-memory latency reduction: {100*(1-b/a):.2f}%.',
          'This measures the effect of the memory policy on this machine, not the complete cross-CPU difference.',
          'Per-run numastat snapshots and raw profiling JSON are retained alongside this summary.', '']
(root / 'COMPARISON.md').write_text('\n'.join(lines))
print(json.dumps(summary))
PY
echo complete > "$OUT/status.txt"
