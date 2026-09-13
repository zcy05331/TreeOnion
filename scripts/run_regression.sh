#!/usr/bin/env bash
# One-shot regression run: every asserting PirTest through `--test all`, then
# the CLI paths that no in-process test can reach (layer_layout_sweep writing
# a profile, merkle_benchmarks consuming it under both planner policies, the
# fallback flag and the mismatch rejection), then the CloudLab chains replayed
# at tiny scale: the benchmark JSON the C++ writers emit fed to the Python
# validators, analyzers and selectors that consume it on the cluster, plus the
# argument-parsing and flag-gating contracts run.py and the archived runs rely
# on. Exits non-zero on any failure.
#
#   scripts/run_regression.sh [path/to/Onion-PIR ...]
#
# With no argument the binary is build-x86_64-benchmark/Onion-PIR (the Rosetta
# x86_64 Benchmark build documented in CLAUDE.md). Several binaries, one per
# ACTIVE_CONFIG, form a configuration matrix: `--test all` runs on each and
# prints the tests that do not apply to that parameter point (the Merkle
# baselines and the ring switch need CONFIG_N2048_K1_COMP resp. K = 1); the
# CLI section runs only on binaries where the Merkle suite applies.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SCRIPTS="$ROOT/scripts"

BINS=("$@")
[[ ${#BINS[@]} -eq 0 ]] && BINS=(build-x86_64-benchmark/Onion-PIR)
for BIN in "${BINS[@]}"; do
  if [[ ! -x "$BIN" ]]; then
    echo "error: binary not found or not executable: $BIN" >&2
    exit 2
  fi
done
WORK="$(mktemp -d "${TMPDIR:-/tmp}/onionpir-regression.XXXXXX")"
# Every step sends its command output to a log under $WORK, so a step that dies
# under set -e would otherwise take its own diagnosis with it: the operator is
# left with the step banner and an exit code. Show the logs the run touched
# last before the directory goes away.
finish() {
  local code=$?
  if [[ $code -ne 0 && -d "$WORK" ]]; then
    local log
    log="$(find "$WORK" -type f -name '*.log' -exec ls -t {} + 2>/dev/null |
           head -n 1)"
    printf '\nregression FAILED (exit %s)\n' "$code" >&2
    if [[ -n "$log" ]]; then
      # The failing step is the last one that wrote a log, so this is normally
      # its output; a step that dies before redirecting anywhere leaves the
      # previous step's log here instead, hence the wording.
      printf 'last log written (%s), final 30 lines:\n\n' "$log" >&2
      tail -n 30 "$log" >&2
    else
      printf 'no logs were written under %s\n' "$WORK" >&2
    fi
  fi
  rm -rf "$WORK"
}
trap finish EXIT

step() { printf '\n==> %s\n' "$*"; }

expect_rejected() {  # expect_rejected <binary> <log> <message> <argv...>
  local BIN="$1" LOG="$2" MESSAGE="$3"; shift 3
  if "$BIN" "$@" > "$LOG" 2>&1; then
    echo "FAILED: '$*' was accepted" >&2; exit 1
  fi
  grep -q -e "$MESSAGE" "$LOG" || {
    echo "FAILED: '$*' was rejected without '$MESSAGE'" >&2; tail -n 3 "$LOG" >&2; exit 1; }
}

run_suite() {  # run_suite <binary>
  local BIN="$1"
  local TAG; TAG="$(basename "$(dirname "$BIN")")"
  local W="$WORK/$TAG"; mkdir -p "$W"
  local ABS_BIN; ABS_BIN="$(cd "$(dirname "$BIN")" && pwd)/$(basename "$BIN")"

  step "[$TAG] in-process regression suite (--test all)"
  "$BIN" --test all --experiments 3 --warmup 1 > "$W/all.log" 2>&1 || {
    tail -n 40 "$W/all.log"; echo "FAILED: --test all ($BIN)" >&2; exit 1; }
  grep -q "Regression suite: .* tests passed" "$W/all.log"
  sed -n '/^Regression suite:/,$p' "$W/all.log"

  # main.cpp runs experiments + warmup iterations and sets TimerLogger's warm-up
  # count from the same flag. Dropping either leaves WARMUP_ITERATIONS above the
  # record count, after which GET_AVG_TIME returns 0, the averages print
  # "Not enough experiments" and the throughput prints inf -- all with exit 0.
  # `pir` is in kRegressionTests, so --test all above already ran it with
  # --experiments 3 --warmup 1 (= 4 iterations); slice that test's section out of
  # the log rather than paying ~2 s for a second run. Parameter-point
  # independent, so it covers every binary in the matrix.
  step "[$TAG] --test pir ran --experiments iterations after --warmup warm-ups"
  awk '/^Running test: /{keep = ($0 == "Running test: pir")} keep' \
    "$W/all.log" > "$W/pir.log"
  local EXPERIMENTS_RUN
  EXPERIMENTS_RUN="$(grep -c '^======================== Experiment ' "$W/pir.log" || true)"
  if [[ "$EXPERIMENTS_RUN" != 4 ]]; then
    echo "FAILED: --experiments 3 --warmup 1 ran $EXPERIMENTS_RUN pir iterations, expected 4" >&2
    exit 1
  fi
  grep -q "Success rate: 4/4" "$W/pir.log"
  grep -q "Online Average Results (after first 1 warm-up runs)" "$W/pir.log"
  if grep -q "Not enough experiments" "$W/pir.log"; then
    echo "FAILED: the online averages had fewer records than the warm-up count" >&2; exit 1
  fi
  grep -Eq '^Server throughput: [0-9]+(\.[0-9]+)? MB/s' "$W/pir.log" || {
    echo "FAILED: server throughput is not a finite number" >&2
    grep -n '^Server throughput' "$W/pir.log" >&2; exit 1; }

  if grep -q "^NOT APPLICABLE merkle_integration" "$W/all.log"; then
    echo "[$TAG] Merkle suite not applicable to this build; CLI section skipped"
    return 0
  fi

  # CLI end to end at 2^8 leaves (H = 8): the sweep writes a profile file,
  # the suite loads it from disk and reports the profiled policy in its JSON.
  local PROFILE="$W/profile-h8.json"
  step "[$TAG] layer_layout_sweep -> $PROFILE"
  # --layer-padding-budget 1.25, not the 1.01 both main.cpp and
  # LayerLayoutSweepOptions default to: with the default the echoed value is the
  # same whether or not main.cpp copies the parsed flag. 1.25 is exact in binary,
  # so it round-trips as "1.25", and it selects the same expansion heights, so
  # the consumers below are unaffected.
  "$BIN" --test layer_layout_sweep --leaf-count 256 --warmup 1 --experiments 2 \
    --trial-seed 7 --layer-padding-budget 1.25 \
    --layout-profile-json "$PROFILE" > "$W/sweep.log" 2>&1
  grep -q '"schema_version": "layer-layout-profile-v1"' "$PROFILE"
  grep -q '"tree_height": 8' "$PROFILE"
  # The parsed --warmup/--experiments/--trial-seed/--layer-padding-budget values
  # are what summarize_cloudlab_profiles.py and select_cuckoo_layout.py check a
  # run against; main.cpp copying them into LayerLayoutSweepOptions is untested
  # in process.
  grep -q '"warmups": 1' "$PROFILE"
  grep -q '"measured_trials": 2' "$PROFILE"
  grep -q '"trial_seed": 7' "$PROFILE"
  grep -q '"padding_budget": 1.25' "$PROFILE"

  step "[$TAG] merkle_benchmarks --layer-layout-policy profiled (profile from disk)"
  "$BIN" --test merkle_benchmarks --benchmark-case merkle_layerwise \
    --leaf-count 256 --warmup 0 --experiments 1 --trial-seed 7 \
    --layer-layout-policy profiled --layer-layout-profile "$PROFILE" \
    --benchmark-json "$W/profiled.json" > "$W/profiled.log" 2>&1
  grep -q '"layer_layout_policy": "profiled"' "$W/profiled.json"
  grep -q '"correctness_passed": true' "$W/profiled.json"
  grep -q '"leaf_count": 256' "$W/profiled.json"
  grep -q '"warmups": 0' "$W/profiled.json"
  grep -q '"measured_trials": 1' "$W/profiled.json"
  grep -q '"trial_seed": 7' "$W/profiled.json"

  step "[$TAG] merkle_benchmarks --layer-layout-policy legacy (all three cases)"
  "$BIN" --test merkle_benchmarks --benchmark-case all \
    --leaf-count 256 --warmup 0 --experiments 1 --trial-seed 7 \
    --benchmark-json "$W/legacy.json" > "$W/legacy.log" 2>&1
  grep -q '"layer_layout_policy": "legacy"' "$W/legacy.json"
  # NB: a bare `[[ ... ]]` does not trip set -e under bash 3.2 (macOS), so every
  # count contract below is written as an if-guard that exits explicitly.
  if [[ "$(grep -c '"correctness_passed": true' "$W/legacy.json")" != 3 ]]; then
    echo "FAILED: --benchmark-case all did not report three correct cases" >&2; exit 1
  fi
  grep -q '"leaf_count": 256' "$W/legacy.json"
  grep -q '"warmups": 0' "$W/legacy.json"
  grep -q '"measured_trials": 1' "$W/legacy.json"
  grep -q '"trial_seed": 7' "$W/legacy.json"

  step "[$TAG] profiled policy with a mismatched tree height is rejected"
  if "$BIN" --test merkle_benchmarks --benchmark-case merkle_layerwise \
       --leaf-count 512 --warmup 0 --experiments 1 \
       --layer-layout-policy profiled --layer-layout-profile "$PROFILE" \
       > "$W/mismatch.log" 2>&1; then
    echo "FAILED: mismatched profile was accepted" >&2; exit 1
  fi
  grep -q "tree_height mismatch" "$W/mismatch.log"

  step "[$TAG] --allow-layout-profile-fallback turns the mismatch into the legacy plan"
  "$BIN" --test merkle_benchmarks --benchmark-case merkle_layerwise \
    --leaf-count 512 --warmup 0 --experiments 1 \
    --layer-layout-policy profiled --layer-layout-profile "$PROFILE" \
    --allow-layout-profile-fallback \
    --benchmark-json "$W/fallback.json" > "$W/fallback.log" 2>&1
  grep -q '"layer_layout_policy": "legacy"' "$W/fallback.json"

  step "[$TAG] a profile whose median disagrees with its samples is rejected"
  sed 's/"median_server_ms": [0-9.e+-]*/"median_server_ms": -1.0/' "$PROFILE" \
    > "$W/profile-drifted.json"
  if "$BIN" --test merkle_benchmarks --benchmark-case merkle_layerwise \
       --leaf-count 256 --warmup 0 --experiments 1 \
       --layer-layout-policy profiled \
       --layer-layout-profile "$W/profile-drifted.json" \
       > "$W/drifted.log" 2>&1; then
    echo "FAILED: drifted profile median was accepted" >&2; exit 1
  fi
  grep -q "does not match" "$W/drifted.log"

  # ---- the CloudLab chains, replayed at 2^8 leaves -------------------------
  # Every Python test in scripts/tests hand-writes its benchmark fixtures and
  # mocks subprocess, so a renamed or dropped field in the C++ JSON writers
  # passes the unit suites and fails hours into a cluster run. These steps feed
  # real binary output to the consumers run_cloudlab_profile_matrix.sh runs.

  step "[$TAG] summarize_cloudlab_profiles / summarize_layer_layout_profile on the Merkle output"
  python3 "$SCRIPTS/summarize_cloudlab_profiles.py" --validate "$W/legacy.json" \
    --trials 1 --leaf-count 256 | grep -q '^valid merkle'
  python3 "$SCRIPTS/summarize_cloudlab_profiles.py" --validate "$W/profiled.json" \
    --trials 1 --leaf-count 256 | grep -q '^valid merkle'
  # Not vacuous: the sample-count contract is what makes an interrupted case
  # fail instead of being summarized.
  if python3 "$SCRIPTS/summarize_cloudlab_profiles.py" --validate "$W/legacy.json" \
       --trials 2 --leaf-count 256 > "$W/badtrials.log" 2>&1; then
    echo "FAILED: a merkle profile with the wrong trial count was accepted" >&2; exit 1
  fi
  python3 "$SCRIPTS/summarize_layer_layout_profile.py" "$PROFILE" "$W/legacy.json" \
    "$W/profiled.json" > "$W/layer-report.txt"
  grep -q 'A/B layerwise (legacy vs profiled):' "$W/layer-report.txt"
  grep -q 'communication unchanged: True' "$W/layer-report.txt"
  if [[ "$(grep -c 'legacy,selected' "$W/layer-report.txt")" != 2 ]]; then
    echo "FAILED: the profile no longer marks two levels as both legacy and selected" >&2
    exit 1
  fi

  step "[$TAG] cuckoo forced-height pilots -> validate -> select_cuckoo_layout"
  for h in 1 2 3; do
    CUCKOO_LAYOUT_HEIGHT=$h BENCHMARK_OUTPUT_JSON="$W/cuckoo-h$h.json" \
      "$BIN" --test cuckoo_bench --leaf-count 256 --warmup 0 --experiments 1 \
      --trial-seed 7 > "$W/cuckoo-h$h.log" 2>&1
    grep -q '"layout_policy": "forced_expansion_height"' "$W/cuckoo-h$h.json"
    grep -q "\"requested_expansion_height\": $h" "$W/cuckoo-h$h.json"
    python3 "$SCRIPTS/summarize_cloudlab_profiles.py" --validate "$W/cuckoo-h$h.json" \
      --trials 1 --leaf-count 256 | grep -q '^valid sampled'
    printf '{"exit_code": 0}\n' > "$W/cuckoo-h$h-process.json"
  done
  python3 "$SCRIPTS/select_cuckoo_layout.py" --output "$W/cuckoo-selection.json" \
    --expected-trials 1 --expected-warmups 0 --leaf-count 256 --heights 1,2,3 \
    "$W/cuckoo-h1.json" "$W/cuckoo-h2.json" "$W/cuckoo-h3.json" > "$W/cuckoo-select.log"
  python3 - "$W/legacy.json" "$W/cuckoo-selection.json" \
    "$W/cuckoo-h1.json" "$W/cuckoo-h2.json" "$W/cuckoo-h3.json" <<'PY'
import json, statistics, sys
merkle, selection, *pilots = (json.load(open(path)) for path in sys.argv[1:])
# Same --leaf-count/--warmup/--experiments/--trial-seed, so the pilots must
# sample the leaves merkle_benchmarks sampled: the cross-baseline comparison
# pairs trials by leaf.
leaves = merkle["workload"]["trial_leaf_indices"]
means = {}
for pilot in pilots:
    assert [s["leaf"] for s in pilot["samples"]] == leaves, "cuckoo pilot leaves differ from merkle_benchmarks"
    means[pilot["requested_expansion_height"]] = statistics.fmean(
        s["server_total_ms"] for s in pilot["samples"])
chosen = selection["decision"]["chosen_expansion_height"]
assert chosen == min(means, key=means.get), f"selected height {chosen} but means are {means}"
PY
  if CUCKOO_LAYOUT_HEIGHT=99 "$BIN" --test cuckoo_bench --leaf-count 256 \
       --warmup 0 --experiments 1 > "$W/cuckoo-h99.log" 2>&1; then
    echo "FAILED: an out-of-range CUCKOO_LAYOUT_HEIGHT was accepted" >&2; exit 1
  fi
  grep -q "CUCKOO_LAYOUT_HEIGHT must be in" "$W/cuckoo-h99.log"

  step "[$TAG] profile directory: peak-RSS wrapper -> validate -> analyze -> summary"
  mkdir -p "$W/p/1gb"
  SUPERRECORD_ENTRY_BITS=256 SUPERRECORD_OUTPUT_JSON="$W/p/1gb/generic.json" \
    SUPERRECORD_SKIP_LEGACY=1 SUPERRECORD_BUNDLE_ARITIES=1,3 SUPERRECORD_MAX_ARITY=3 \
    SUPERRECORD_MAX_FRONTIER=0 SUPERRECORD_H1_FRONTIER=0 SUPERRECORD_MAX_RESPONSES=1 \
    SUPERRECORD_KEY_MODE=fixed SUPERRECORD_FIXED_KEY_HEIGHT=11 \
    SUPERRECORD_MIN_NOISE_MARGIN=0 SUPERRECORD_MEMORY_BYTES=2147483648 \
    python3 "$SCRIPTS/read_process_peak_memory.py" --log "$W/p/1gb/generic.log" \
      --output "$W/p/1gb/generic-process.json" -- \
      "$BIN" --test tree_superrecord_bench --leaf-count 256 --experiments 2 \
      --warmup 1 --trial-seed 7 > /dev/null
  BENCHMARK_OUTPUT_JSON="$W/p/1gb/tree_g32.json" \
    python3 "$SCRIPTS/read_process_peak_memory.py" --log "$W/p/1gb/tree_g32.log" \
      --output "$W/p/1gb/tree_g32-process.json" -- \
      "$BIN" --test tree_bench_g32 --leaf-count 256 --experiments 2 --warmup 1 \
      --trial-seed 7 > /dev/null
  python3 "$SCRIPTS/read_process_peak_memory.py" --log "$W/p/1gb/flat.log" \
    --output "$W/p/1gb/flat-process.json" -- \
    "$BIN" --test merkle_benchmarks --benchmark-case merkle_paths --leaf-count 256 \
    --experiments 2 --warmup 1 --trial-seed 7 --benchmark-json "$W/p/1gb/flat.json" > /dev/null
  BENCHMARK_OUTPUT_JSON="$W/p/1gb/cuckoo_tuned.json" \
    python3 "$SCRIPTS/read_process_peak_memory.py" --log "$W/p/1gb/cuckoo_tuned.log" \
      --output "$W/p/1gb/cuckoo_tuned-process.json" -- \
      "$BIN" --test cuckoo_bench --leaf-count 256 --experiments 2 --warmup 1 \
      --trial-seed 7 > /dev/null
  python3 "$SCRIPTS/summarize_cloudlab_profiles.py" --validate "$W/p/1gb/generic.json" \
    --trials 2 --leaf-count 256 | grep -q '^valid generic'
  python3 "$SCRIPTS/summarize_cloudlab_profiles.py" --validate "$W/p/1gb/tree_g32.json" \
    --trials 2 --leaf-count 256 | grep -q '^valid sampled'
  python3 "$SCRIPTS/summarize_cloudlab_profiles.py" --validate "$W/p/1gb/flat.json" \
    --trials 2 --leaf-count 256 | grep -q '^valid merkle'
  python3 "$SCRIPTS/summarize_cloudlab_profiles.py" --validate "$W/p/1gb/cuckoo_tuned.json" \
    --trials 2 --leaf-count 256 | grep -q '^valid sampled'
  if python3 "$SCRIPTS/summarize_cloudlab_profiles.py" --validate "$W/p/1gb/generic.json" \
       --trials 16 --leaf-count 256 > "$W/generic-badtrials.log" 2>&1; then
    echo "FAILED: a generic profile with the wrong trial count was accepted" >&2; exit 1
  fi
  grep -q "samples count 2 != expected 16" "$W/generic-badtrials.log"
  # Analyze a copy: the -analysis.json sibling must not land in the summarized tree.
  mkdir -p "$W/analysis"; cp "$W/p/1gb/generic.json" "$W/analysis/generic.json"
  python3 "$SCRIPTS/analyze_superrecord_sweep.py" "$W/analysis/generic.json" > "$W/analyze.log"
  python3 - "$W/analysis/generic-analysis.json" <<'PY'
import json, sys
analysis = json.load(open(sys.argv[1]))
assert analysis["run_class"] == "smoke_only", analysis["run_class"]
assert analysis["measured_blocks"] == 2, analysis["measured_blocks"]
control = analysis["fastest_eligible_measured_h1"]
assert control, "no measured h1 control in the analysis"
bundled = [row["label"] for row in analysis["candidates"]
           if row["shape"].get("bundle_arity") not in (None, 1)]
assert bundled, "no bundled candidate was measured"
for label in bundled:
    assert control in analysis["comparisons"][label], (label, analysis["comparisons"][label])
PY
  python3 "$SCRIPTS/summarize_cloudlab_profiles.py" --directory "$W/p" > "$W/summary.log"
  python3 - "$W/p/profile_summary.json" "$W/p/stage_samples.csv" <<'PY'
import csv, json, sys
summary = json.load(open(sys.argv[1]))
for case, stage in (("generic", "server_total_ms"), ("tree_g32", "server_total_ms"),
                    ("cuckoo_tuned", "server_total_ms"), ("merkle_flat", "server_compute_ms")):
    rows = [row for row in summary["summaries"]
            if row["case"] == case and row["stage"] == stage]
    assert rows, f"no {stage} rows for case {case}"
    for row in rows:
        assert row["count"] == 2, (case, row["variant"], row["count"])
        peak = row["peak_rss_bytes"]
        assert isinstance(peak, int) and peak > 0, (case, peak)
with open(sys.argv[2]) as handle:
    sources = {row["source_file"] for row in csv.DictReader(handle)}
assert sources, "no stage samples collected"
assert not any(s.endswith("-process.json") for s in sources), sources
PY

  step "[$TAG] tree benchmark JSON stages, correctness and communication"
  BENCHMARK_OUTPUT_JSON="$W/tree_compress.json" "$BIN" --test tree_compress_bench \
    --leaf-count 256 --experiments 2 --warmup 0 > "$W/tree_compress.log" 2>&1
  # tree_bench fixes a = 7, so it needs >= 2^18 leaves, as build_cloudlab_profiles.sh does.
  BENCHMARK_OUTPUT_JSON="$W/tree_bench.json" "$BIN" --test tree_bench \
    --leaf-count 262144 --experiments 2 --warmup 0 > "$W/tree_bench.log" 2>&1
  python3 - "$W/p/1gb/tree_g32.json" "$W/tree_compress.json" "$W/tree_bench.json" <<'PY'
import json, sys
# The stage names scripts/analyze_wisconsin_profiles.py reads for the tree family.
STAGES = {"query_expand", "selector_completion", "pyramid", "scan_core", "scan_finalize",
          "fold", "rotate", "project", "pack", "switch"}
EXPECTED = {"legacy-tree-g32": ("mvp", {"scan_combined"}, False),
            "legacy-tree-g32-compressed": ("compressed", {"scan_combined",
                "response_ring_switch_key_switch", "response_ring_switch_modswitch"}, True),
            "legacy-tree-g1": ("mvp", {"scan_combined"}, False)}
for path in sys.argv[1:]:
    data = json.load(open(path))
    mode, diagnostics, compressed = EXPECTED[data["scheme"]]
    assert data["mode"] == mode, (path, data["mode"])
    assert len(data["samples"]) == 2, (path, len(data["samples"]))
    for sample in data["samples"]:
        assert sample["correct"] is True, path
        assert set(sample["stages_ms"]) == STAGES, (path, sorted(sample["stages_ms"]))
        # TimerLogger::getLastTime returns 0.0 for a section whose TIME_START /
        # TIME_END pair was removed or renamed: zeros are the silent failure mode.
        assert all(value > 0 for value in sample["stages_ms"].values()), (path, sample["stages_ms"])
        assert diagnostics <= set(sample["diagnostics_ms"]), (path, sorted(sample["diagnostics_ms"]))
    communication = data["communication"]
    assert communication["actual_response_bytes_per_trial"] > 0, path
    assert (communication["modeled_compression_key_bytes"] > 0) is compressed, (path, communication)
    if data["scheme"] == "legacy-tree-g32":
        for key in ("plaintext_modulus", "response_modulus", "min_noise_budget_bits", "N0", "b"):
            assert key in data["metadata"], (path, key)
PY

  step "[$TAG] tree_bench_g32 takes trials/warmups/seed from the flags, else its defaults"
  grep -q '"leaf_count": 256' "$W/p/1gb/tree_g32.json"
  grep -q '"warmups": 1' "$W/p/1gb/tree_g32.json"
  grep -q '"measured_trials": 2' "$W/p/1gb/tree_g32.json"
  grep -q '"trial_seed": 7' "$W/p/1gb/tree_g32.json"
  if [[ "$(grep -c '"trial":' "$W/p/1gb/tree_g32.json")" != 2 ]]; then
    echo "FAILED: --experiments 2 did not produce two tree_bench_g32 trial rows" >&2; exit 1
  fi
  # The other direction: an archived flag-less run keeps the frozen defaults
  # that tests.h states, and the analysis scripts reject a mismatch.
  BENCHMARK_OUTPUT_JSON="$W/g32-defaults.json" "$BIN" --test tree_bench_g32 \
    --leaf-count 256 > "$W/g32-defaults.log" 2>&1
  grep -q '"warmups": 3' "$W/g32-defaults.json"
  grep -q '"measured_trials": 16' "$W/g32-defaults.json"
  grep -q '"trial_seed": 8390880541405295199' "$W/g32-defaults.json"
  if [[ "$(grep -c '"trial":' "$W/g32-defaults.json")" != 16 ]]; then
    echo "FAILED: a flag-less tree_bench_g32 run no longer measures its default 16 trials" >&2
    exit 1
  fi

  # Both binary-backed modules default to the Rosetta build directory, so on a
  # campaign host that builds into build-native they skip themselves and their
  # coverage is silently lost. Naming them here points both at the binary under
  # test; naming the two modules rather than discovering a pattern also keeps
  # the step independent of whatever else lives under scripts/tests, and shares
  # one interpreter (10 s, against 13 s for two separate runs).
  step "[$TAG] benchmark JSON contract and gadget driver against this binary"
  ( cd "$ROOT" && ONIONPIR_BENCH_BIN="$ABS_BIN" \
      python3 -m unittest scripts.tests.test_benchmark_json_contract \
                          scripts.tests.test_gadget_configs_e2e )

  step "[$TAG] argument validation"
  if "$BIN" --test merkle_benchmarks --layer-layout-policy profiled \
       --leaf-count 256 --warmup 0 --experiments 1 > "$W/noprof.log" 2>&1; then
    echo "FAILED: profiled policy without a profile was accepted" >&2; exit 1
  fi
  grep -q "requires" "$W/noprof.log"
  if "$BIN" --test layer_layout_sweep --leaf-count 256 > "$W/nojson.log" 2>&1; then
    echo "FAILED: sweep without --layout-profile-json was accepted" >&2; exit 1
  fi
  if "$BIN" --test pir --bogus-flag > "$W/bogus.log" 2>&1; then
    echo "FAILED: unknown option was accepted" >&2; exit 1
  fi
  grep -q "Unknown option" "$W/bogus.log"
  if "$BIN" --test tree_bench --leaf-count 1000 > "$W/badleaf.log" 2>&1; then
    echo "FAILED: non-power-of-two --leaf-count was accepted" >&2; exit 1
  fi
  grep -q "power of two" "$W/badleaf.log"

  # Partial consumption and sign rejection: with strtoull/atoi in place of
  # parse_size_argument, `--experiments abc` would run zero experiments and exit
  # 0 printing "Success rate: 0/0", and `10abc` would silently run ten. Each
  # case fails while parsing, before any test starts.
  expect_rejected "$BIN" "$W/exp-abc.log" "requires a non-negative integer" \
    --test pir --experiments abc
  expect_rejected "$BIN" "$W/exp-suffix.log" "is outside the size_t range" \
    --test pir --experiments 10abc
  expect_rejected "$BIN" "$W/warmup-neg.log" "requires a non-negative integer" \
    --test pir --warmup -1
  expect_rejected "$BIN" "$W/exp-novalue.log" "requires a value" \
    --test pir --experiments
  expect_rejected "$BIN" "$W/seed-overflow.log" "requires a non-negative integer" \
    --test pir --trial-seed 18446744073709551616
  expect_rejected "$BIN" "$W/case-bad.log" "--benchmark-case must be" \
    --test merkle_benchmarks --leaf-count 256 --benchmark-case bogus
  expect_rejected "$BIN" "$W/policy-bad.log" "--layer-layout-policy must be" \
    --test merkle_benchmarks --leaf-count 256 --layer-layout-policy bogus
  expect_rejected "$BIN" "$W/unknown-test.log" "Unknown test: nosuchtest" --test nosuchtest
  expect_rejected "$BIN" "$W/sweep-zero.log" "--experiments must be positive" \
    --test layer_layout_sweep --leaf-count 256 --experiments 0 \
    --layout-profile-json "$W/never-written.json"
  if [[ -e "$W/never-written.json" ]]; then
    echo "FAILED: a rejected sweep still wrote its profile" >&2; exit 1
  fi
  # run.py forwards --no-compress verbatim for every -t run; the flag exists
  # only for that path (stderr may carry macOS lscpu noise, so only the exit
  # code is checked).
  "$BIN" --test cpu_info --no-compress > "$W/nocompress.log" 2>&1
}

for BIN in "${BINS[@]}"; do
  run_suite "$BIN"
done

echo
echo "regression OK"
