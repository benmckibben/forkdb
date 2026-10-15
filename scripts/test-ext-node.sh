#!/usr/bin/env bash
# Test the forkdb loadable extension from Node's built-in node:sqlite, on Linux, macOS and Windows (Git Bash).
# Needs: a C compiler, curl and python for the SQLite headers (for the build), and a Node whose node:sqlite has DatabaseSync.loadExtension (24 is what CI uses).
# The fsync-counting tests (nosync) need an LD_PRELOAD interposer and run on Linux only; elsewhere they are left out, and the script says so.
# Usage: scripts/test-ext-node.sh        NODE=/path/to/node overrides the node binary.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
cd "$root"
node=${NODE:-node}

echo "Testing forkdb loadable extension from node:sqlite..."
echo "node version: $("$node" --version) on $(uname -s)"
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
FK_EXT_SO=$(scripts/build-ext.sh --path)
export FK_EXT_SO

# Explicit file lists: with no arguments node --test would also run every .js file under test/, the helpers and the worker among them.
shopt -s nullglob
files=(test/ext_node_test.js)
if [ "$(uname -s)" = Linux ]; then
    echo "Building fsync_count interposer..."
    gcc -shared -fPIC -O2 -o "$root/build/ext/fsync_count.so" test/fsync_count.c -ldl
    export FK_FSYNC_COUNT_SO="$root/build/ext/fsync_count.so"
    files+=(test/ext_node_*_test.js)
else
    echo "NOT RUN on $(uname -s): test/ext_node_nosync_test.js (it counts fsync calls with an LD_PRELOAD interposer, which is Linux only)"
fi
echo "Running Node tests: ${files[*]}"
if ! "$node" --no-warnings --test --test-reporter=spec "${files[@]}"; then
    # A file that dies as a whole (a crash in the library, an exception while loading) is reported as test failed and nothing else: run each file again directly, for its own output.
    for f in "${files[@]}"; do
        echo "== $f, run directly (output of a failing run):"
        "$node" --no-warnings "$f" 2>&1 | tail -n 60 || true
    done
    exit 1
fi

echo "all Node extension tests passed"
