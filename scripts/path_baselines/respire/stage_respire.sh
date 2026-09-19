#!/usr/bin/env bash
set -euo pipefail

SCRIPT_DIR="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
REPO_ROOT="$(cd "${SCRIPT_DIR}/../../.." && pwd)"
STAGE_ROOT="${REPO_ROOT}/.benchmarks/respire-path"

ARGS=(--adapter-dir "${SCRIPT_DIR}" --stage-root "${STAGE_ROOT}")
if [[ -n "${RESPIRE_SOURCE_DIR:-}" ]]; then
  ARGS+=(--source-dir "${RESPIRE_SOURCE_DIR}")
fi
python3 "${SCRIPT_DIR}/stage_respire.py" "${ARGS[@]}"
