#!/usr/bin/env bash
# The version is written in five places; they must agree: the extension (forkdb_info), the CLI (--version), the Python package (__version__ and pyproject.toml) and the npm package.
#   scripts/check-versions.sh
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
cd "$root"
ext=$(grep -o 'result_text(ctx, "[0-9][0-9.]*"' ext/forkdb_ext.c | head -1 | grep -o '[0-9][0-9.]*' || true)
cli=$(grep -o 'define FORKDB_VERSION "[0-9][0-9.]*"' cli/forkdb.c | grep -o '[0-9][0-9.]*' || true)
pyinit=$(grep -o '__version__ = "[0-9][0-9.]*"' python/forkdb/__init__.py | grep -o '[0-9][0-9.]*' || true)
pytoml=$(grep -o '^version = "[0-9][0-9.]*"' python/pyproject.toml | grep -o '[0-9][0-9.]*' || true)
node=$(grep -o '"version": "[0-9][0-9.]*"' node/package.json | grep -o '[0-9][0-9.]*' || true)
printf "extension %s | cli %s | python __init__ %s | python pyproject %s | npm %s\n" "$ext" "$cli" "$pyinit" "$pytoml" "$node"
bad=0
for v in "$cli" "$pyinit" "$pytoml" "$node"; do [ -n "$ext" ] && [ "$v" = "$ext" ] || bad=1; done
[ "$bad" -eq 0 ] || { echo "FAIL: the version is not the same in all five places"; exit 1; }
echo "all five agree: $ext"
