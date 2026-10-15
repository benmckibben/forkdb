#!/usr/bin/env bash
# The README says the extension loads into SQLite 3.24 and later. This builds the SQLite shell from the 3.24.0 amalgamation (the oldest supported) and from 3.23.1
# (the newest unsupported), and checks that the extension works in the first and is refused, with a clear message, by the second. Linux only.
# The amalgamations come from sqlite.org over https. The SHA-256 sums below were measured when this script was written (sqlite.org publishes SHA3 sums, not
# SHA-256, so they could not be compared with a published value): they guard against a changed download, not against one that was wrong from the start.
# Needs: gcc, curl, sha256sum, python3 (to unzip).
set -euo pipefail

here=$(cd "$(dirname "$0")/.." && pwd)
cd "$here"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

scripts/build-ext.sh
so=$(scripts/build-ext.sh --path)

shell_for() { # version number, year, sha256: prints the path of a built sqlite3 shell
  local v=$1 y=$2 sum=$3 zip="$work/sqlite-$1.zip"
  curl -fsSL -o "$zip" "https://www.sqlite.org/$y/sqlite-amalgamation-$v.zip"
  local got
  got=$(sha256sum "$zip" | cut -d' ' -f1)
  [ "$got" = "$sum" ] || { echo "checksum mismatch for SQLite $v: expected $sum, got $got" >&2; exit 1; }
  python3 -c "import sys, zipfile; zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])" "$zip" "$work"
  # -DSQLITE_USE_URI=1: forkdb_open_db returns a URI, and this shell passes no URI flag of its own
  gcc -O1 -w -DSQLITE_USE_URI=1 -DSQLITE_THREADSAFE=1 -DSQLITE_ENABLE_LOAD_EXTENSION=1 "$work/sqlite-amalgamation-$v/shell.c" "$work/sqlite-amalgamation-$v/sqlite3.c" \
    -ldl -lpthread -lm -o "$work/sqlite3-$v"
  echo "$work/sqlite3-$v"
}

echo "== SQLite 3.23.1 (below the minimum): the extension must refuse to load, and say why"
new_shell=$(shell_for 3230100 2018 4239a1f69e5721d07d9a374eb84d594225229e54be4ee628da2995f4315d8dfc)
out=$("$new_shell" :memory: ".load $so" "select 1;" 2>&1 || true)
echo "$out"
grep -q "forkdb needs SQLite 3.24 or later, this host has 3.23.1" <<< "$out" || { echo "FAIL: 3.23.1 did not refuse the extension with the expected message"; exit 1; }

echo "== SQLite 3.24.0 (the minimum): the extension must work, across two processes"
sh=$(shell_for 3240000 2018 ad68c1216c3a474cf360c7581a4001e952515b3649342100f2d7ca7c8e313da6)
store=$work/s.fk
one=$(cat <<SQL
.load $so
.bail on
select 'version', sqlite_version();
select 'info', value from forkdb_info where key = 'vfs';
select forkdb_init('$store');
attach forkdb_open_db('$store', 'main') as m;
create table m.t(a integer primary key, b text);
with recursive c(x) as (select 1 union all select x + 1 from c where x < 100) insert into m.t select x, 'x' from c;
select 'main rows', count(*) from m.t;
select 'fork', typeof(forkdb_fork('$store', 'main', 'a'));
attach forkdb_open_db('$store', 'a') as a;
update a.t set b = 'A' where a <= 10;
select 'a edited', count(*) from a.t where b = 'A';
select 'main edited', count(*) from m.t where b = 'A';
select 'integrity m', integrity_check from pragma_integrity_check('m');
select 'integrity a', integrity_check from pragma_integrity_check('a');
select 'snapshot', typeof(forkdb_snapshot('$store', 'main', 's'));
select 'discard', forkdb_discard('$store', 's');
select 'close', forkdb_close('$store');
SQL
)
out1=$(printf '%s\n' "$one" | "$sh" :memory: 2>&1 || true)
echo "$out1"
two=$(cat <<SQL
.load $so
.bail on
select forkdb_init('$store');
attach forkdb_open_db('$store', 'main') as m;
attach forkdb_open_db('$store', 'a') as a;
select 'reopened main rows', count(*) from m.t;
select 'reopened a edited', count(*) from a.t where b = 'A';
select 'reopened main edited', count(*) from m.t where b = 'A';
SQL
)
out2=$(printf '%s\n' "$two" | "$sh" :memory: 2>&1 || true)
echo "$out2"
bad=0
want() { grep -qx -- "$2" <<< "$1" || { echo "FAIL: expected the line \"$2\""; bad=1; }; }
want "$out1" "version|3.24.0"
want "$out1" "info|forkdb"
want "$out1" "main rows|100"
want "$out1" "fork|integer"
want "$out1" "a edited|10"
want "$out1" "main edited|0"
want "$out1" "integrity m|ok"
want "$out1" "integrity a|ok"
want "$out1" "snapshot|integer"
want "$out2" "reopened main rows|100"
want "$out2" "reopened a edited|10"
want "$out2" "reopened main edited|0"
if grep -q "^Error" <<< "$out1$out2"; then echo "FAIL: an error in the output"; bad=1; fi
[ "$bad" -eq 0 ] || exit 1
echo "all oldest-SQLite tests passed"
