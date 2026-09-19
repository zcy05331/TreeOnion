#!/usr/bin/env bash
set -euo pipefail

UPSTREAM_COMMIT="930063c5aefc441244abb4890fdf35383f3aa956"
TREEPIR_ROOT="${1:?usage: stage_vbpir_pbc.sh /path/to/TreePIR [target-dir]}"
TARGET_DIR="${2:-.benchmarks/vbpir-pbc}"
SOURCE_DIR="$TREEPIR_ROOT/VBPIR_PBC"

if [[ ! -d "$SOURCE_DIR" ]]; then
  echo "missing VBPIR_PBC source: $SOURCE_DIR" >&2
  exit 1
fi

actual_commit="$(git -C "$TREEPIR_ROOT" rev-parse HEAD)"
if [[ "$actual_commit" != "$UPSTREAM_COMMIT" ]]; then
  echo "TreePIR checkout must be at $UPSTREAM_COMMIT, got $actual_commit" >&2
  exit 1
fi

rm -rf "$TARGET_DIR"
mkdir -p "$(dirname "$TARGET_DIR")"
cp -R "$SOURCE_DIR" "$TARGET_DIR"

python3 "$(dirname "$0")/patch_vbpir_pbc.py" "$TARGET_DIR"

echo "staged VBPIR_PBC at $TARGET_DIR from TreePIR $UPSTREAM_COMMIT"
