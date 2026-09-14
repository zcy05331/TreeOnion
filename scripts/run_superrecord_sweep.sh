#!/usr/bin/env bash
# Reproducible local sweep, raw artifacts, memory metadata, and paired analysis.
set -euo pipefail
SCRIPT_DIR="$(cd -- "$(dirname -- "${BASH_SOURCE[0]}")" && pwd)"
exec python3 "$SCRIPT_DIR/run_superrecord_sweep.py" "$@"
