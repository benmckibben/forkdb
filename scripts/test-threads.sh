#!/usr/bin/env bash
# Build and run the multi-threaded stress test for forkdb under ThreadSanitizer.
# Needs gcc, curl, sha256sum and python3 (the first run downloads the pinned SQLite,
# then compiles it with -fsanitize=thread, which takes about 90 s).
#
#   scripts/test-threads.sh                 runs SEED 1 2 3 with THREADS=4, then SEED 4 with THREADS=8
#   scripts/test-threads.sh quick           4 threads, one seed only
#
# Without the VFS/fkx mutex the test fails (TSan data-race reports, "disk I/O error", heap
# use-after-free); with it, every run must be clean: zero TSan reports, zero failed checks.
set -euo pipefail

here=$(cd "$(dirname "$0")" && pwd)
root=$(cd "$here/.." && pwd)
cd "$root" || exit 1

quick=0
[ "${1:-}" = quick ] && quick=1

# ── fetch and compile the pinned SQLite ───────────────────────────────
spike/fetch-sqlite.sh > /dev/null
src=spike/vendor/sqlite-amalgamation-3500000

out="$root/build"
mkdir -p "$out"

sqlite_obj="$out/sqlite3-tsan.o"
if [ ! -f "$sqlite_obj" ]; then
  echo "compiling SQLite with ThreadSanitizer (one-time, ~90 s)..."
  # SQLITE_THREADSAFE=1 is the default; be explicit.  DQS=0 disables
  # double-quoted string literals (common-sense mode).
  gcc -O1 -g -fsanitize=thread -std=gnu11 \
    -DSQLITE_THREADSAFE=1 -DSQLITE_OMIT_LOAD_EXTENSION -DSQLITE_DQS=0 \
    -c "$src/sqlite3.c" -o "$sqlite_obj.tmp"
  mv "$sqlite_obj.tmp" "$sqlite_obj"
  echo "built $sqlite_obj"
fi

# ── compile the test binary ───────────────────────────────────────────
test_bin="$out/thread_test"
warn=(-Wall -Wextra -std=gnu11)
san=(-fsanitize=thread -O1 -g)
inc=(-I"$src" -Ifkx -Ivfs -Istore)
gcc "${warn[@]}" "${san[@]}" "${inc[@]}" \
  test/thread_test.c \
  fkx/fkx.c fkx/fkx_nosession.c \
  vfs/fkvfs.c \
  store/fkstore.c store/io_file.c store/io_prealloc.c \
  "$sqlite_obj" \
  -lpthread -lm -ldl \
  -o "$test_bin"
echo "built $test_bin"

# ── TSan on WSL2 needs setarch -R (disable address-space layout randomisation) ──
tsan_wrapper=()
if setarch "$(uname -m)" -R /bin/true 2>/dev/null; then
  tsan_wrapper=(setarch "$(uname -m)" -R)
fi

# ── run one seed combination ─────────────────────────────────────────
run_one() {
  local threads=$1 seed=$2
  local log="$out/thr-t${threads}-s${seed}.log"
  echo "== THREADS=$threads SEED=$seed =="
  TSAN_OPTIONS=halt_on_error=0:exitcode=66 \
    timeout 300 \
    env THREADS="$threads" SEED="$seed" ITERS="${ITERS:-200}" \
    "${tsan_wrapper[@]}" "$test_bin" \
    > "$log" 2>&1
  local rc=$?
  if [ "$rc" -ne 0 ]; then
    echo "  FAILED (rc=$rc)"
    echo "  --- first 40 lines ---"
    head -40 "$log"
    echo "  --- end ---"
  else
    local rpt
    rpt=$(grep -c 'WARNING: ThreadSanitizer' "$log" || true)
    echo "  PASSED (rc=$rc, $rpt TSan warning(s))"
  fi
  return "$rc"
}

# ── main ──────────────────────────────────────────────────────────────
failed_seeds=()
rc=0

if [ "$quick" = 1 ]; then
  run_one 4 1 || { rc=1; }
else
  for seed in 1 2 3; do
    run_one 4 "$seed" || { rc=1; failed_seeds+=("seed $seed (THREADS=4)"); }
  done
  run_one 8 4 || { rc=1; failed_seeds+=("seed 4 (THREADS=8)"); }
fi

if [ "$rc" -ne 0 ]; then
  echo ""
  echo "=== FAILED SEEDS ==="
  for f in "${failed_seeds[@]}"; do echo "  $f"; done
  exit 1
fi

echo "all thread tests passed"
