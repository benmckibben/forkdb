#!/usr/bin/env bash
# Test the forkdb loadable extension from Node's built-in node:sqlite.
# Needs: gcc, curl+sha256sum for the SQLite headers (for the build), and a Node whose node:sqlite has DatabaseSync.loadExtension (24 is what CI uses).
# Usage: scripts/test-ext-node.sh        NODE=/path/to/node overrides the node binary.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
cd "$root"
node=${NODE:-node}

echo "Testing forkdb loadable extension from node:sqlite..."
echo "node version: $("$node" --version)"
echo -n "node:sqlite DatabaseSync.loadExtension: "
if "$node" --no-warnings -e "const {DatabaseSync}=require('node:sqlite'); process.exit(typeof DatabaseSync.prototype.loadExtension==='function'?0:1)" 2>/dev/null; then
    echo "YES"
else
    echo "NO"
    echo "FAIL: this node has no node:sqlite DatabaseSync.loadExtension, so the suite cannot run (it is not skipped)"
    exit 1
fi
"$node" --no-warnings -e "const {DatabaseSync}=require('node:sqlite'); const d=new DatabaseSync(':memory:'); console.log('node:sqlite SQLite version:', d.prepare('select sqlite_version() v').get().v)"

echo "Building extension..."
scripts/build-ext.sh

export FK_EXT_SO="$root/build/ext/forkdb.so"

echo "Building fsync_count interposer..."
gcc -shared -fPIC -O2 -o "$root/build/ext/fsync_count.so" test/fsync_count.c -ldl
export FK_FSYNC_COUNT_SO="$root/build/ext/fsync_count.so"

# Explicit file list: with no arguments node --test would also run every .js file under test/, the helpers and the worker among them.
shopt -s nullglob
files=(test/ext_node_test.js test/ext_node_*_test.js)
echo "Running Node tests: ${files[*]}"
"$node" --no-warnings --test --test-reporter=spec "${files[@]}"

echo "all Node extension tests passed"
