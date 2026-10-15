#!/usr/bin/env bash
# Test the forkdb loadable extension from better-sqlite3 (a Node binding that bundles its own SQLite), on Linux, macOS and Windows (Git Bash).
# Needs: gcc, curl+sha256sum for the SQLite headers (for the build), Node >= 22, and npm with network access: it installs the PINNED better-sqlite3 into build/bsq
# (build/ is not committed). better-sqlite3 ships prebuilt binaries for Linux, macOS and Windows, so nothing is compiled.
# Usage: scripts/test-ext-bsq.sh        NODE=/path/to/node and NPM=/path/to/npm override the binaries.
set -euo pipefail

BSQ_VERSION=13.0.3

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
cd "$root"
node=${NODE:-node}
npm=${NPM:-npm}

echo "Testing forkdb loadable extension from better-sqlite3 $BSQ_VERSION..."
echo "node version: $("$node" --version)"

echo "Installing better-sqlite3@$BSQ_VERSION into build/bsq..."
mkdir -p build/bsq
if [ ! -f build/bsq/package.json ]; then echo '{"private":true,"name":"forkdb-bsq-test"}' > build/bsq/package.json; fi
(cd build/bsq && "$npm" install --no-audit --no-fund --no-save --save-exact "better-sqlite3@$BSQ_VERSION" > install.log 2>&1) || { tail -20 build/bsq/install.log; echo "FAIL: npm install better-sqlite3@$BSQ_VERSION failed"; exit 1; }
got=$(sed -n 's/.*"version": "\(.*\)".*/\1/p' build/bsq/node_modules/better-sqlite3/package.json | head -1)
if [ "$got" != "$BSQ_VERSION" ]; then echo "FAIL: installed better-sqlite3 $got, wanted $BSQ_VERSION"; exit 1; fi

echo "Building extension..."
scripts/build-ext.sh

FK_EXT_SO=$(scripts/build-ext.sh --path)
export FK_EXT_SO
# better-sqlite3 is built with SQLITE_USE_URI=0 and reads this variable once, when the addon loads: it must be in the environment before node starts.
export SQLITE_USE_URI=1

# Explicit file lists: with no arguments node --test would also run the helper files under test/.
# The fsync-counting tests (nosync) need an LD_PRELOAD interposer and run on Linux only; elsewhere they are left out, and the script says so.
files=(test/ext_bsq_test.js test/ext_bsq_port_test.js test/ext_bsq_threads_test.js)
if [ "$(uname -s)" = Linux ]; then
    echo "Building fsync_count interposer..."
    gcc -shared -fPIC -O2 -o "$root/build/ext/fsync_count.so" test/fsync_count.c -ldl
    export FK_FSYNC_COUNT_SO="$root/build/ext/fsync_count.so"
    files+=(test/ext_bsq_nosync_test.js)
else
    echo "NOT RUN on $(uname -s): test/ext_bsq_nosync_test.js (it counts fsync calls with an LD_PRELOAD interposer, which is Linux only)"
fi
echo "Running better-sqlite3 tests: ${files[*]}"
"$node" --no-warnings --test --test-reporter=spec "${files[@]}"

echo "all better-sqlite3 extension tests passed"
