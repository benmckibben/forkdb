#!/usr/bin/env bash
# Build and run the VFS spike tests: SQLite on the durable store. Needs gcc, curl, sha256sum and python3 (the first run compiles
# the pinned SQLite twice, which takes about a minute).
#   scripts/test-vfs.sh          both SQLite builds under the sanitizers, then the harness self-check (mutants)
#   scripts/test-vfs.sh quick    both builds, a sparse power-loss sweep, no mutants
set -euo pipefail

here=$(cd "$(dirname "$0")/.." && pwd)
limit=${LIMIT:-120} # seconds: every test binary runs under a hard timeout
quick=0
[ "${1:-}" = quick ] && quick=1
[ $quick = 1 ] && export VFS_STEP=7

"$here/vfs/build.sh" sqlite
for v in batch stock; do
  echo "== SQLite build: $v (address and undefined-behaviour sanitizers on)"
  SAN=1 "$here/vfs/build.sh" test "$v" | tail -1
  timeout "$limit" "$here/vfs/build/vfs_test-$v-san" | grep -E '^(SQLite|T11|   [0-9]+ I/O events|checks|  FAIL|  VIOLATION)'
done
[ $quick = 1 ] && exit 0

echo "== do the tests catch broken VFS code? (each mutant must make the tests fail)"
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
src=$here/spike/vendor/sqlite-amalgamation-3500000
caught=0
total=0
# mutant NAME OLD NEW: replace the text OLD (which must occur exactly once in vfs/fkvfs.c) with NEW, build the unchanged tests
# against it, and require them to fail
mutant() {
  local name=$1 old=$2 new=$3
  total=$((total + 1))
  mkdir -p "$tmp/m"
  python3 - "$here/vfs/fkvfs.c" "$tmp/m/fkvfs.c" "$old" "$new" <<'EOF'
import sys
s = open(sys.argv[1]).read()
if s.count(sys.argv[3]) != 1:
    sys.exit('mutant text must occur exactly once: ' + sys.argv[3][:60])
open(sys.argv[2], 'w').write(s.replace(sys.argv[3], sys.argv[4]))
EOF
  cp "$here/vfs/fkvfs.h" "$tmp/m/"
  gcc -w -std=gnu11 -O2 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_ENABLE_BATCH_ATOMIC_WRITE -I"$src" -I"$tmp/m" -I"$here/store" \
    "$here/vfs/vfs_test.c" "$tmp/m/fkvfs.c" "$here/store/fkstore.c" "$here/store/io_file.c" "$here/test/simdisk.c" \
    "$here/vfs/build/sqlite3-batch.o" -o "$tmp/m/t" -lpthread -lm
  if timeout "$limit" "$tmp/m/t" > "$tmp/m/out" 2>&1; then
    echo "  SURVIVED: $name"
  else
    caught=$((caught + 1))
    echo "  caught:   $name  ($(grep -cE 'FAIL|VIOLATION' "$tmp/m/out" || true) failing lines)"
  fi
}

mutant 'an atomic commit that discards the pages and says OK' 'int rc = publish(f, 0);' 'int rc = (discard(f), SQLITE_OK);'
mutant 'a transaction published only half' 'for (uint32_t i = 0; i < n && rc == SQLITE_OK; i++) {' 'for (uint32_t i = 0; i < (n > 1 ? n / 2 : n) && rc == SQLITE_OK; i++) {'
mutant 'dedupe drops every page' 'memcmp(cur, stage_find(&f->st, pg), PG) == 0' '1'
mutant 'dedupe off (rollbacks cost a commit)' 'if (dedupe) {' 'if (0) {'
mutant 'a shrink is forgotten' 'e = fk_txn_commit(s, ns);' 'e = fk_txn_commit(s, cs > ns ? cs : ns);'
mutant 'no read-your-writes' 'if (p) { memcpy(out, p, PG); return 0; }' 'if (0 && p) { memcpy(out, p, PG); return 0; }'
mutant 'a writer is not held off by readers' 'if (b->nshared > 1) return SQLITE_BUSY;' 'if (0) return SQLITE_BUSY;'
mutant 'a failed atomic commit is swallowed and its pages dropped' 'if (rc != SQLITE_OK) { v->st.commit_failed++; return SQLITE_IOERR_COMMIT_ATOMIC; }' 'if (rc != SQLITE_OK) { v->st.commit_failed++; discard(f); return SQLITE_OK; }'
mutant 'a failed sync is reported as OK' $'  int rc = publish(f, 1);\n  if (rc == SQLITE_OK) f->v->st.sync_publish++; else f->v->st.sync_publish_errors++;\n  return rc;' $'  int rc = publish(f, 1);\n  if (rc == SQLITE_OK) f->v->st.sync_publish++; else f->v->st.sync_publish_errors++;\n  return SQLITE_OK;'
mutant 'the WAL pragma is not refused' 'sqlite3_stricmp(a[1], "journal_mode") == 0 && sqlite3_stricmp(a[2], "wal") == 0' '0'
mutant 'synchronous=OFF is not refused' 'sqlite3_stricmp(a[1], "synchronous") == 0 && (' '0 && ('
mutant 'a truncate keeps the staged pages beyond it' 'stage_drop_from(&f->st, pages);' '(void)pages;'
mutant 'a no-op publish leaves its staging in place' 'if (rc == SQLITE_OK) discard(f);' 'if (rc == SQLITE_OK && !dirty(f)) discard(f);'
mutant 'a failed commit keeps staging whose pages the store already took' 'fk_txn_abort(s); discard(f); goto done;' 'fk_txn_abort(s); goto done;'
mutant 'the zero-fill pass overwrites pages already handed to the store' 'if (stage_has(&f->st, pg)) continue;' 'if (stage_find(&f->st, pg)) continue;'
mutant 'a branch id is not checked' 'if (parse_branch(zName, &b) != 0 || !fk_branch_alive(v->s, b)) return SQLITE_CANTOPEN;' 'if (parse_branch(zName, &b) != 0) return SQLITE_CANTOPEN;'
echo "   $caught of $total mutants caught"
[ "$caught" = "$total" ] || { echo "harness gap: a mutant survived"; exit 1; }
echo "all VFS tests passed"
