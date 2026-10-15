#!/usr/bin/env bash
# CLI regression test: checks every subcommand against bad store paths (missing, junk, no-dir).
# The init command legitimately creates a store; the others must return exit code 1 with a clear
# error message, never segfault.  A happy-path sanity round ensures the tests are meaningful.
#   scripts/test-cli.sh
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
cd "$root" || exit 1

# ---- build -----------------------------------------------------------
vfs/build.sh sqlite > /dev/null
scripts/build-step4.sh cli > /dev/null
CLI=$root/build/forkdb

# ---- temp dir --------------------------------------------------------
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT

MISSING=$tmp/missing.fk
JUNK=$tmp/junk.fk
NODIR=$tmp/no/such/dir/x.fk

# prepare junk file (5000 bytes of 'x')
head -c 5000 /dev/zero | tr '\0' x > "$JUNK"
JUNK_ORIG=$tmp/junk-orig.fk
cp "$JUNK" "$JUNK_ORIG"

# ---- helpers ---------------------------------------------------------
failures=0
checks=0

fail() {
  echo "FAIL[$checks]: $*" >&2
  failures=$((failures + 1))
}

check_cmd() {
  # $1=command-name  $2=path  $3=extra-arg1 $4=extra-arg2 ...
  local cmd=$1
  shift
  local path=$1
  shift
  checks=$((checks + 1))

  local stderr_file=$tmp/stderr-$cmd-$checks
  local stdout_file=$tmp/stdout-$cmd-$checks

  set +e
  timeout 20 "$CLI" "$cmd" "$path" "$@" </dev/null >"$stdout_file" 2>"$stderr_file"
  local rc=$?
  set -e

  # exit status must be exactly 1
  if [ "$rc" -ne 1 ]; then
    fail "$cmd $path: exit status $rc (want 1)"
    head -3 "$stderr_file" | while IFS= read -r line; do echo "  stderr: $line"; done
    return
  fi

  # stderr first line must start with 'forkdb: <cmd>: ' and contain the path
  local first_line
  first_line=$(head -1 "$stderr_file")
  case "$first_line" in
    "forkdb: $cmd: "*)
      # ok prefix
      ;;
    *)
      fail "$cmd $path: stderr line 1 does not match 'forkdb: $cmd: <message>': got '$first_line'"
      return
      ;;
  esac
  if ! grep -qF "$path" <<< "$first_line"; then
    fail "$cmd $path: stderr line 1 does not contain path '$path': '$first_line'"
    return
  fi

  # stdout must be empty
  if [ -s "$stdout_file" ]; then
    fail "$cmd $path: stdout is not empty"
    head -3 "$stdout_file" | while IFS= read -r line; do echo "  stdout: $line"; done
    return
  fi
}

check_init_create() {
  # init MISSING must exit 0 and create the file
  checks=$((checks + 1))
  local stderr_file=$tmp/stderr-init-missing-$checks
  local stdout_file=$tmp/stdout-init-missing-$checks

  set +e
  timeout 20 "$CLI" init "$MISSING" </dev/null >"$stdout_file" 2>"$stderr_file"
  local rc=$?
  set -e

  if [ "$rc" -ne 0 ]; then
    fail "init MISSING: exit status $rc (want 0)"
    head -3 "$stderr_file" | while IFS= read -r line; do echo "  stderr: $line"; done
    return
  fi
  if [ ! -f "$MISSING" ]; then
    fail "init MISSING: file was not created"
    return
  fi
}

check_no_create() {
  # $1=cmd $2=path ...   (the path did not exist before, assert it still does not)
  local path=$2
  if [ -f "$path" ]; then
    fail "$*: file '$path' was unexpectedly created"
  fi
}

check_junk_unchanged() {
  if ! cmp -s "$JUNK" "$JUNK_ORIG"; then
    fail "junk file was modified"
  fi
}

# Assert the MISSING-case stderr contains 'no store at' (ENOENT wording, not ECORRUPT/ESTORE/etc)
check_missing_wording() {
  # $1=command-name $2=path (MISSING)
  local cmd=$1 path=$2
  local stderr_file=$tmp/stderr-$cmd-${FUNCNAME[0]}
  local stdout_file=$tmp/stdout-$cmd-${FUNCNAME[0]}
  checks=$((checks + 1))
  set +e
  timeout 20 "$CLI" "$cmd" "$path" "${@:3}" </dev/null >"$stdout_file" 2>"$stderr_file"
  local rc=$?
  set -e
  if [ "$rc" -ne 1 ]; then
    fail "$cmd $path (wording check): exit status $rc (want 1)"
    return
  fi
  local first_line
  first_line=$(head -1 "$stderr_file")
  if ! grep -qF "no store at" <<< "$first_line"; then
    fail "$cmd $path (wording check): expected 'no store at' in '$first_line'"
  fi
}

# ---- bad-path tests: each cmd × (MISSING, JUNK, NODIR) ---------------
# only check_no_create for MISSING (the path never existed).
# For JUNK we verify the bytes are unchanged at the end.
# For NODIR the parent directory does not exist, so the file won't be created.

for bad in "$MISSING" "$JUNK" "$NODIR"; do
  # init: only JUNK and NODIR (MISSING is legitimate, tested separately)
  if [ "$bad" != "$MISSING" ]; then
    check_cmd init "$bad"
  fi

  check_cmd list   "$bad"
  [ "$bad" = "$MISSING" ] && check_missing_wording list "$bad"
  check_cmd info   "$bad"
  [ "$bad" = "$MISSING" ] && check_missing_wording info "$bad"
  [ "$bad" = "$MISSING" ] && check_no_create info "$bad"

  check_cmd fork   "$bad" b1
  [ "$bad" = "$MISSING" ] && check_missing_wording fork "$bad" b1
  [ "$bad" = "$MISSING" ] && check_no_create fork "$bad" b1

  check_cmd snapshot "$bad" s1
  [ "$bad" = "$MISSING" ] && check_missing_wording snapshot "$bad" s1
  [ "$bad" = "$MISSING" ] && check_no_create snapshot "$bad" s1

  check_cmd diff   "$bad" b1
  [ "$bad" = "$MISSING" ] && check_missing_wording diff "$bad" b1
  [ "$bad" = "$MISSING" ] && check_no_create diff "$bad" b1

  check_cmd promote "$bad" b1
  [ "$bad" = "$MISSING" ] && check_missing_wording promote "$bad" b1
  [ "$bad" = "$MISSING" ] && check_no_create promote "$bad" b1

  check_cmd undo   "$bad" b1
  [ "$bad" = "$MISSING" ] && check_missing_wording undo "$bad" b1
  [ "$bad" = "$MISSING" ] && check_no_create undo "$bad" b1

  check_cmd discard "$bad" b1
  [ "$bad" = "$MISSING" ] && check_missing_wording discard "$bad" b1
  [ "$bad" = "$MISSING" ] && check_no_create discard "$bad" b1

  check_cmd reset  "$bad" b1 main
  [ "$bad" = "$MISSING" ] && check_missing_wording reset "$bad" b1 main
  [ "$bad" = "$MISSING" ] && check_no_create reset "$bad" b1 main

  check_cmd gc     "$bad"
  [ "$bad" = "$MISSING" ] && check_missing_wording gc "$bad"
  [ "$bad" = "$MISSING" ] && check_no_create gc "$bad"

  check_cmd compact "$bad"
  [ "$bad" = "$MISSING" ] && check_missing_wording compact "$bad"
  [ "$bad" = "$MISSING" ] && check_no_create compact "$bad"

  check_cmd sql    "$bad" main "select 1"
  [ "$bad" = "$MISSING" ] && check_missing_wording sql "$bad" main "select 1"
  [ "$bad" = "$MISSING" ] && check_no_create sql "$bad" main

  check_cmd shell  "$bad" main
  [ "$bad" = "$MISSING" ] && check_missing_wording shell "$bad" main
  [ "$bad" = "$MISSING" ] && check_no_create shell "$bad" main
done

# init MISSING special case (checked separately: must succeed and create the file)
check_init_create

# junk file unchanged after all commands
check_junk_unchanged

# --version: prints the version, needs no store, and --json gives the same in JSON, whichever order the flags come in; and it is the version the extension reports.
for want in 'forkdb 0.1.0|--version' 'forkdb 0.1.0|-V' 'forkdb 0.1.0|-v' '{"version":"0.1.0"}|--json --version' '{"version":"0.1.0"}|--version --json'; do
  checks=$((checks + 1))
  expected=${want%%|*}; args=${want#*|}
  # shellcheck disable=SC2086
  got=$("$CLI" $args 2>&1 </dev/null) || fail "forkdb $args: exit status $?"
  [ "$got" = "$expected" ] || fail "forkdb $args printed '$got', want '$expected'"
done
checks=$((checks + 1))
ext_version=$(grep -o 'result_text(ctx, "[0-9][0-9.]*"' ext/forkdb_ext.c | head -1 | grep -o '[0-9][0-9.]*' || true)
cli_version=$(grep -o 'define FORKDB_VERSION "[0-9][0-9.]*"' cli/forkdb.c | grep -o '[0-9][0-9.]*' || true)
if [ -z "$ext_version" ] || [ "$ext_version" != "$cli_version" ]; then fail "the CLI's version ('$cli_version') is not the extension's ('$ext_version')"; fi

# A store from before format version 4 is refused with a message that says so (this path used to be remapped to -1 and print nothing useful),
# and the file is not touched.
OLD=$tmp/old.fk
cp "$root/test/data/v2-compacted.fk" "$OLD"
cp "$OLD" "$tmp/old-orig.fk"
for c in list info gc; do
  check_cmd "$c" "$OLD"
  checks=$((checks + 1))
  if ! grep -qF "older than format version 4" <<< "$("$CLI" "$c" "$OLD" 2>&1 </dev/null || true)"; then
    fail "$c on a version 2 store: message does not say it is older than format version 4"
  fi
done
cmp -s "$OLD" "$tmp/old-orig.fk" || fail "a refused version 2 store was modified"

# ---- store-file lock -----------------------------------------------
# Verify the CLI refuses to open a store that another process holds an exclusive flock on,
# and that the file is not modified and the lock is released afterwards.

check_locked() {
  # $1=command-name  $2=path  $3..extra-args (like main "select 1")
  local cmd=$1 path=$2
  shift 2
  checks=$((checks + 1))

  local stderr_file=$tmp/stderr-locked-$cmd-$checks
  local stdout_file=$tmp/stdout-locked-$cmd-$checks

  # Build the command to run inside a flock-held subshell.
  # The lock is held on $path, so the CLI process will hit the exclusive flock conflict.
  set +e
  timeout 30 flock -s "$path" env LC_ALL=C "$CLI" "$cmd" "$path" "$@" </dev/null >"$stdout_file" 2>"$stderr_file"
  local rc=$?
  set -e

  # exit status must be exactly 1
  if [ "$rc" -ne 1 ]; then
    fail "flocked $cmd $path: exit status $rc (want 1)"
    head -3 "$stderr_file" | while IFS= read -r line; do echo "  stderr: $line"; done
    return
  fi

  local first_line
  first_line=$(head -1 "$stderr_file")
  case "$first_line" in
    "forkdb: $cmd: "*)
      # ok prefix
      ;;
    *)
      fail "flocked $cmd $path: stderr line 1 does not match 'forkdb: $cmd: <message>': got '$first_line'"
      return
      ;;
  esac
  if ! grep -qF "$path" <<< "$first_line"; then
    fail "flocked $cmd $path: stderr line 1 does not contain path '$path': '$first_line'"
    return
  fi
  if ! grep -qF "open in another process" <<< "$first_line"; then
    fail "flocked $cmd $path: stderr line 1 does not contain 'open in another process': '$first_line'"
    return
  fi

  # stdout must be empty
  if [ -s "$stdout_file" ]; then
    fail "flocked $cmd $path: stdout is not empty"
    head -3 "$stdout_file" | while IFS= read -r line; do echo "  stdout: $line"; done
    return
  fi
}

if ! command -v flock > /dev/null 2>&1; then
  echo "fatal: flock(1) is not installed; cannot test store-file locking" >&2
  exit 1
fi

LOCKED=$tmp/locked.fk
"$CLI" init "$LOCKED" > /dev/null 2>&1
cp "$LOCKED" "$tmp/locked-orig.fk"

check_locked list "$LOCKED"
check_locked info "$LOCKED"
check_locked gc   "$LOCKED"
check_locked sql  "$LOCKED" main "select 1"

# assert the store file was not modified
cmp -s "$LOCKED" "$tmp/locked-orig.fk" || fail "locked file was modified"

# assert the lock is released after flock returns
checks=$((checks + 1))
if ! "$CLI" list "$LOCKED" > /dev/null 2>&1; then
  fail "could not list $LOCKED after lock was released"
fi

# ---- happy-path sanity -----------------------------------------------
SANITY=$tmp/sanity.fk

sanity_check() {
  checks=$((checks + 1))
  "$CLI" "$@" > /dev/null 2>&1 || fail "$* (exit $?)"
}

sanity_check init "$SANITY"
sanity_check fork "$SANITY" feat
sanity_check list "$SANITY"
# list should show both main and feat
if grep -cq main < <("$CLI" list "$SANITY" 2>/dev/null) && \
   grep -cq feat < <("$CLI" list "$SANITY" 2>/dev/null); then
  : # ok
else
  fail "list $SANITY: expected both main and feat branches"
fi
sanity_check info "$SANITY" main
sanity_check info "$SANITY" feat

# ---- results ---------------------------------------------------------
if [ "$failures" -eq 0 ]; then
  echo "all cli tests passed ($checks checks)"
else
  echo "$failures failure(s) in $checks checks"
  exit 1
fi
