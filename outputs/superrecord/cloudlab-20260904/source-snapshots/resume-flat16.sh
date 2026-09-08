#!/usr/bin/env bash
set -euo pipefail
RUN_ROOT="$HOME/generic-superrecord-bench-20260904"
# Let the already running 100-trial Layerwise process and its RSS writer finish.
while kill -0 10119 2>/dev/null; do sleep 3; done
while [[ ! -f "$RUN_ROOT/outputs/profiling/1gb/layerwise-process.json" ]]; do sleep 1; done
kill -TERM 9803
kill -CONT 9803
printf '%s\n' generic-profile-flat16-20260904 > "$RUN_ROOT/metadata/active-session.txt"
exec bash "$RUN_ROOT/source/scripts/run_cloudlab_profile_matrix.sh" "$RUN_ROOT" >> "$RUN_ROOT/metadata/matrix-controller.log" 2>&1
