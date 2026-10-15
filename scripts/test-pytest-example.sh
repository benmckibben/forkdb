#!/usr/bin/env bash
# The pytest example (examples/pytest-branch-per-test): builds the portable extension, installs pytest and pytest-xdist into build/pylib (no system
# packages, no virtualenv needed), and runs the shop test suite serially, with 4 workers and with the stores on tmpfs, then the isolation canaries
# in every mode. Needs gcc, python3 whose sqlite3 allows load_extension, pip.
set -euo pipefail
here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
cd "$root"
ex=examples/pytest-branch-per-test

scripts/build-ext.sh
lib="$root/build/pylib"
if ! PYTHONPATH="$lib" python3 -c 'import pytest, xdist' 2>/dev/null; then
  python3 -m pip install --quiet --target "$lib" pytest pytest-xdist
fi
export PYTHONPATH="$lib" PYTHONDONTWRITEBYTECODE=1
python3 --version
python3 -c 'import sqlite3, pytest, xdist; print("sqlite", sqlite3.sqlite_version, "pytest", pytest.__version__, "xdist", xdist.__version__)'

shm=$(mktemp -d /dev/shm/fk-pytest-XXXXXX 2>/dev/null || mktemp -d)
trap 'rm -rf "$shm"' EXIT

cd "$root/$ex"
run() {
  echo "== $*"
  python3 -m pytest -p no:cacheprovider -q "$@"
}
run
run -n 4
run --forkdb-dir="$shm"
run -n 4 --forkdb-dir="$shm" --forkdb-max-mb=1
canary/run_canaries.sh
echo "all pytest example tests passed"
