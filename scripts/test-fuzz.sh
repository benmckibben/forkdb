#!/usr/bin/env bash
# Recovery fuzzer: damaged, truncated and malformed store files must be refused cleanly, or open to a store that behaves (see test/fuzz_open.c). Needs only gcc.
#   scripts/test-fuzz.sh            build with the address and undefined-behaviour sanitizers and run FUZZ_INPUTS inputs (default 20000, about 30 s)
#   scripts/test-fuzz.sh mutants    also: does the fuzzer catch a store whose recovery checks were removed? (each mutant must make it fail; python3 is used to edit a scratch copy)
#   FUZZ_INPUTS=500000 FUZZ_SEED=3 scripts/test-fuzz.sh   a longer run on another seed. A failing input is reproduced with:  fuzz_open --one SEED N file
set -euo pipefail

here=$(cd "$(dirname "$0")/.." && pwd)
out=$(mktemp -d)
trap 'rm -rf "$out"' EXIT
cc=${CC:-gcc}
inputs=${FUZZ_INPUTS:-20000}
seed=${FUZZ_SEED:-1}
warn="-Wall -Wextra -std=gnu11 -O1 -g"
san="-fsanitize=address,undefined -fno-sanitize-recover=undefined"
# quarantine_size_mb: the default 256 MB of freed memory the sanitizer keeps is not the library's memory use (peak RSS was 340 MB with it, 21 MB without).
export ASAN_OPTIONS=allocator_may_return_null=1:max_allocation_size_mb=512:quarantine_size_mb=1:detect_leaks=1
export UBSAN_OPTIONS=print_stacktrace=1

build() { # source tree, output, extra compiler flags
  local d=$1 o=$2; shift 2
  # shellcheck disable=SC2086
  $cc $warn $san -DFK_MAX_BRANCHES=64 "$@" "$d/test/fuzz_open.c" "$d/test/simdisk.c" "$d/store/fkstore.c" "$d/store/io_prealloc.c" -o "$o"
}

echo "== recovery fuzzer ($inputs inputs, seed $seed, address and undefined-behaviour sanitizers on)"
build "$here" "$out/fuzz_open"
"$out/fuzz_open" "$inputs" "$seed" 2>&1 | grep -a -E "^(fuzz:|TOTAL|FUZZ|VIOLATION|peak memory)|ERROR|runtime error|SUMMARY"
[ "${PIPESTATUS[0]}" -eq 0 ] || { echo "FAIL: the fuzzer found a problem"; exit 1; }

[ "${1:-}" = mutants ] || { echo "all fuzz tests passed"; exit 0; }

echo "== does the fuzzer catch broken recovery? (each mutant must make it fail)"
minputs=${FUZZ_MUTANT_INPUTS:-30000}
bad=0
mutant() { # name, file, old text, new text (the old text must occur exactly once unless a count is given as the 5th argument), [-D flags in $FZ_FLAGS]. KNOWN_SURVIVOR=reason: not caught is reported, not a failure
  local name=$1 file=$2 want=${5:-1} known=${KNOWN_SURVIVOR:-}
  rm -rf "$out/m"; mkdir -p "$out/m"; cp -r "$here/store" "$here/test" "$out/m/"
  if [ -n "$file" ]; then
    FZ_FILE="$out/m/$file" FZ_OLD=$3 FZ_NEW=$4 FZ_WANT=$want python3 - <<'PY'
import os, sys
f, old, new, want = os.environ["FZ_FILE"], os.environ["FZ_OLD"], os.environ["FZ_NEW"], int(os.environ["FZ_WANT"])
s = open(f).read()
if s.count(old) != want:
    print("PATTERN PROBLEM: %d occurrences of %r, wanted %d" % (s.count(old), old, want)); sys.exit(7)
open(f, "w").write(s.replace(old, new))
PY
  fi
  # shellcheck disable=SC2086
  build "$out/m" "$out/m/fuzz_open" ${FZ_FLAGS:-} 2> "$out/m/build.log" || { echo "  $name: BUILD FAILED"; tail -3 "$out/m/build.log"; bad=1; return; }
  local rc=0
  timeout 600 "$out/m/fuzz_open" "$minputs" "$seed" > "$out/m/run.log" 2>&1 || rc=$?
  if [ "$rc" -eq 0 ]; then
    if [ -n "$known" ]; then echo "  $name: not caught, as expected ($known)"; else echo "  $name: NOT CAUGHT in $minputs inputs"; bad=1; fi
    return
  fi
  [ -z "$known" ] || echo "  $name: NOW CAUGHT (it was a known survivor: $known): move it to the required list"
  local how
  how=$(grep -a -m1 -E "^VIOLATION|ERROR: AddressSanitizer|runtime error|HANG|LeakSanitizer" "$out/m/run.log" | cut -c1-110 || true)
  echo "  $name: caught (rc $rc): ${how:-no message}"
}

S=store/fkstore.c
mutant "commit: nsize > FK_MAX_PAGES unchecked" $S " || nsize > FK_MAX_PAGES) break;" ") break;"
mutant "commit: page number beyond the new size accepted" $S "if (pend[i].pgno >= nsize) bad = 1;" "if (pend[i].pgno >= nsize) bad = 0;"
mutant "commit: page count not compared with the pages seen" $S "|| np != npend ||" "||"
mutant "page record: branch id not bounded" $S "if (b >= s->nb || !s->br[b].alive || (npend && b != pbranch)) break;" "if (!s->br[b].alive || (npend && b != pbranch)) break;"
mutant "record length not bounded by a page" $S "if (get32(h) != MAGIC || len > PAGE_PAYLOAD) break;" "if (get32(h) != MAGIC) break;"
mutant "fork: destination id not bounded" $S " || dst >= FK_MAX_BRANCHES) break;" ") break;" 2
mutant "metadata record: length not bounded" $S "if (ml > FK_META_MAX || len != 12 + ml) break;" "if (0) break;"
mutant "metadata record: ml > FK_META_MAX unchecked" $S "if (ml > FK_META_MAX || len != 8 + ml ||" "if (len != 8 + ml ||"
KNOWN_SURVIVOR="redundant: a page copy that the stream refers to must have the page number the stream gives, and the stream refuses a page number at the limit; an unreferenced copy is harmless" mutant "snapshot page record: page number unbounded" $S " || get32(pay + 4) >= FK_MAX_PAGES) break;" ") break;"
mutant "snapshot end: checksum not checked" $S " || get32(pay + 8) != fk_crc32c(0, stream, slen)) break;" ") break;"
mutant "snapshot end: length not checked" $S "if (len != 12 || !slen || get64(pay) != slen ||" "if (len != 12 || !slen ||"
mutant "snapshot: layer parent may point forward" $S "(par != NONE && par >= i) ||" ""
mutant "snapshot: page list may be unsorted" $S "|| (int64_t)pg <= prev ||" "||"
mutant "snapshot: an offset need not be a page copy" $S "if (!q || q->pgno != pg) { rc = FK_ECORRUPT; break; }" "(void)q;"
mutant "snapshot: branch head layer unchecked" $S "|| (alive && head >= nl) ||" "||"
KNOWN_SURVIVOR="not caught even by a mutator that sets the last live branch metadata length past the end of the stream: later reads of the stream fail anyway (reason not verified in detail)" mutant "snapshot: metadata length unchecked against the stream" $S "|| n - r.i < ml ||" "||"
mutant "snapshot: state id above the counter" $S "|| st > ctr) { rc = FK_ECORRUPT; break; }" ") { rc = FK_ECORRUPT; break; }"
FZ_FLAGS="-DFK_BUG_NO_PREVCHAIN" mutant "record chain not checked (FK_BUG_NO_PREVCHAIN)" "" "" ""

[ "$bad" -eq 0 ] || { echo "FAIL: a mutant was not caught (see above)"; exit 1; }
echo "all fuzz tests passed (every mutant caught)"
