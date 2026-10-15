#!/usr/bin/env bash
# Test the forkdb loadable extension.
# Needs: gcc, python3 whose sqlite3 module allows load_extension, curl+sha256sum for the SQLite headers.
# Usage: scripts/test-ext.sh
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
cd "$root"

echo "Testing forkdb loadable extension..."

# Prevent Python from writing bytecode files
export PYTHONDONTWRITEBYTECODE=1

# Print versions
echo "python3 version: $(python3 --version)"
python3 -c "import sqlite3; print('sqlite3 version:', sqlite3.sqlite_version)"

# Check if Connection has enable_load_extension
echo -n "Connection.enable_load_extension: "
if python3 -c "import sqlite3; exit(0 if hasattr(sqlite3.Connection, 'enable_load_extension') else 1)"; then
    echo "YES"
else
    echo "NO"
    echo "FAIL: sqlite3.Connection has no enable_load_extension method"
    exit 1
fi

# Build the extension
echo "Building extension..."
scripts/build-ext.sh

# Set the extension path
FK_EXT_SO=$(scripts/build-ext.sh --path)
export FK_EXT_SO

# Build the fsync-counting interposer
echo "Building fsync_count interposer..."
gcc -shared -fPIC -O2 -o "$root/build/ext/fsync_count.so" test/fsync_count.c -ldl

# Set the interposer path
export FK_FSYNC_COUNT_SO="$root/build/ext/fsync_count.so"

# Run the tests
echo "Running Python tests..."
python3 test/ext_py_test.py -v

echo "all extension tests passed"