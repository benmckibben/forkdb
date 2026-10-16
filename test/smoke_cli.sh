#!/usr/bin/env bash
# Smoke test of a built forkdb command-line binary, the one that goes into a release archive (it is not the test binary of scripts/test-cli.sh).
#   test/smoke_cli.sh PATH-TO-forkdb VERSION
# Checks the version, a store made and forked, SQL on a branch that the other branch does not see, a diff, a promote, a compaction, and that a file that is not a store is refused untouched.
set -euo pipefail
bin=${1:?usage: smoke_cli.sh PATH-TO-forkdb VERSION}
version=${2:?usage: smoke_cli.sh PATH-TO-forkdb VERSION}
# The test works in a scratch directory: make the path of the binary absolute first (a relative one, as the workflow passes it, would not resolve there).
bin=$(cd "$(dirname "$bin")" && pwd)/$(basename "$bin")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
cd "$tmp"
n=0
fail() { echo "FAIL: $*" >&2; exit 1; }
check() { n=$((n + 1)); }

check; [ "$("$bin" --version)" = "forkdb $version" ] || fail "--version printed '$("$bin" --version)', want 'forkdb $version'"
check; [ "$("$bin" --json --version)" = "{\"version\":\"$version\"}" ] || fail "--json --version"

check; "$bin" init s.fk > /dev/null || fail "init"
check; "$bin" fork s.fk feat > /dev/null || fail "fork"
list=$("$bin" list s.fk)
check; grep -q '^main ' <<< "$list" || fail "list does not show main: $list"
check; grep -q '^feat ' <<< "$list" || fail "list does not show feat: $list"

check; "$bin" sql s.fk feat "create table t(a); insert into t values (1),(2),(3)" --write > /dev/null || fail "sql --write on feat"
check; [ "$("$bin" sql s.fk feat 'select count(*) from t' | tail -1)" = 3 ] || fail "feat does not hold the 3 rows"
check; if out=$("$bin" sql s.fk main 'select count(*) from t' 2>&1); then fail "main sees the table of feat: $out"; fi
check; grep -q 'no such table' <<< "$out" || fail "main: expected 'no such table', got: $out"
check; "$bin" diff s.fk feat | grep -q '^t .*only_here' || fail "diff does not show t as only on feat"

check; "$bin" promote s.fk feat > /dev/null || fail "promote"
check; [ "$("$bin" sql s.fk main 'select sum(a) from t' | tail -1)" = 6 ] || fail "main does not hold the promoted rows"
check; "$bin" compact s.fk --min-bytes=1 > /dev/null || fail "compact"
check; [ "$("$bin" sql s.fk main 'select sum(a) from t' | tail -1)" = 6 ] || fail "after the compaction main does not hold the rows"

head -c 5000 /dev/zero | tr '\0' x > junk.fk
cp junk.fk junk-orig.fk
check; if "$bin" list junk.fk > /dev/null 2>&1; then fail "list accepted a file that is not a store"; fi
check; cmp -s junk.fk junk-orig.fk || fail "a file that is not a store was modified"

echo "CLI smoke test passed: $n checks ($bin $version)"
