#!/usr/bin/env bash
# Tests of the release scripts, in a scratch repository: release-guard.sh (each way a tag can be wrong, and the one way it can be right), release-notes.sh, package-release.sh.
#   scripts/test-release.sh
# Every failing case must fail with the message that says why: a case that fails for another reason does not count.
set -euo pipefail
root=$(cd "$(dirname "$0")/.." && pwd)
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
checks=0
bad=0
fail() { echo "  FAIL $*"; bad=$((bad + 1)); }
ok() { checks=$((checks + 1)); }

# ---- a scratch repository with the version in its five places, a changelog, and a stand-in for gh ----
repo=$tmp/repo
mkdir -p "$repo/scripts" "$repo/ext" "$repo/cli" "$repo/python/forkdb" "$repo/node" "$tmp/bin"
cp "$root/scripts/check-versions.sh" "$root/scripts/release-guard.sh" "$root/scripts/release-notes.sh" "$repo/scripts/"
write_versions() { # VERSION [which place to set differently]
  local v=$1
  echo "  sqlite3_result_text(ctx, \"$v\", -1, 0);" > "$repo/ext/forkdb_ext.c"
  echo "#define FORKDB_VERSION \"$v\"" > "$repo/cli/forkdb.c"
  echo "__version__ = \"$v\"" > "$repo/python/forkdb/__init__.py"
  echo "version = \"$v\"" > "$repo/python/pyproject.toml"
  echo "  \"version\": \"$v\"," > "$repo/node/package.json"
}
write_changelog() { # the whole text
  printf '%s\n' "$1" > "$repo/CHANGELOG.md"
}
good_changelog='# Changelog

## Unreleased

Nothing yet.

## [0.1.0] - 2026-10-16

Intro line.

### The store

- Something (#1).

## [0.0.9] - 2026-10-01

- Older.

[0.1.0]: https://example.invalid/0.1.0'
write_versions 0.1.0
write_changelog "$good_changelog"
# gh: answers the CI-runs query with the conclusion in GH_STUB_CONCLUSION, but only if it is asked about the right workflow and commit
cat > "$tmp/bin/gh" <<'STUB'
#!/usr/bin/env bash
case "$*" in
  *"actions/workflows/ci.yml/runs?head_sha=$GH_STUB_SHA&"*) echo "${GH_STUB_CONCLUSION:-success}" ;;
  *) echo "gh stub: unexpected query: $*" >&2; exit 1 ;;
esac
STUB
chmod +x "$tmp/bin/gh"
git -C "$repo" init -q -b main
git -C "$repo" add -A
git -C "$repo" -c user.name=t -c user.email=t@example.invalid commit -q -m "first"
git -C "$repo" update-ref refs/remotes/origin/main HEAD
sha=$(git -C "$repo" rev-parse HEAD)

# guard NAME EXPECT(ok or a message that must be in the output) [VAR=value ...]
guard() {
  local name=$1 expect=$2; shift 2
  local out rc=0
  out=$(cd "$repo" && env REF_TYPE=tag REF_NAME=v0.1.0 DEFAULT_BRANCH=main GITHUB_REPOSITORY=o/r GH_STUB_SHA="${COMMIT_SHA:-$sha}" PATH="$tmp/bin:$PATH" "$@" scripts/release-guard.sh 2>&1) || rc=$?
  if [ "$expect" = ok ]; then
    if [ "$rc" -eq 0 ]; then ok; else fail "$name: exited $rc: $out"; fi
  else
    if [ "$rc" -ne 0 ] && grep -qF -- "$expect" <<< "$out"; then ok; else fail "$name: want a failure saying '$expect'; got rc=$rc: $out"; fi
  fi
}

echo "== release-guard.sh"
guard "the right tag" ok
guard "a branch run needs no tag" ok REF_TYPE=branch REF_NAME=main
guard "a branch run still prints the version" ok REF_TYPE=branch REF_NAME=main
guard "tag without the v" "is not of the form" REF_NAME=0.1.0
guard "tag with two numbers" "is not of the form" REF_NAME=v0.1
guard "pre-release tag" "is not of the form" REF_NAME=v0.1.0-rc1
guard "tag of another version" "does not match the version 0.1.0" REF_NAME=v0.1.1
guard "tag with trailing text" "is not of the form" "REF_NAME=v0.1.0 "
guard "CI failed" "latest completed run: failure" GH_STUB_CONCLUSION=failure
guard "CI cancelled" "latest completed run: cancelled" GH_STUB_CONCLUSION=cancelled
guard "no CI run on the commit" "latest completed run: none" GH_STUB_CONCLUSION=none
guard "the query is about another commit" "could not read the CI runs" GH_STUB_SHA=0000000000000000000000000000000000000000 COMMIT_SHA="$sha"

# the version strings disagree
echo "version = \"0.1.1\"" > "$repo/python/pyproject.toml"
guard "pyproject.toml disagrees" "not the same in all five places"
write_versions 0.1.0

# the commit is not on the default branch
git -C "$repo" checkout -q -b side
echo x > "$repo/side.txt"; git -C "$repo" add side.txt
git -C "$repo" -c user.name=t -c user.email=t@example.invalid commit -q -m side
side=$(git -C "$repo" rev-parse HEAD)
guard "commit not on main" "is not reachable from main" COMMIT_SHA="$side"
guard "commit on main, from a checkout of the side branch" ok COMMIT_SHA="$sha"
git -C "$repo" checkout -q main

# the changelog
write_changelog "$(printf '%s\n' "$good_changelog" | sed 's/^## \[0.1.0\] - 2026-10-16$/## Unreleased/')"
guard "no version heading" "no heading '## [0.1.0] - YYYY-MM-DD'"
write_changelog "$(printf '%s\n' "$good_changelog" | sed 's/^## \[0.1.0\] - 2026-10-16$/## [0.1.0]/')"
guard "heading without a date" "no heading '## [0.1.0] - YYYY-MM-DD'"
write_changelog "$(printf '%s\n' "$good_changelog" | sed 's/^## \[0.1.0\] - 2026-10-16$/## [0.1.00] - 2026-10-16/')"
guard "heading of a version that only looks like it" "no heading '## [0.1.0] - YYYY-MM-DD'"
write_changelog "# Changelog

## [0.1.0] - 2026-10-16

## [0.0.9] - 2026-10-01

- Older."
guard "empty section" "nothing under '## [0.1.0]'"
write_changelog "$good_changelog"

echo "== release-notes.sh"
want='Intro line.

### The store

- Something (#1).'
got=$(cd "$repo" && scripts/release-notes.sh 0.1.0)
if [ "$got" = "$want" ]; then ok; else fail "notes: got '$got'"; fi
got=$(cd "$repo" && scripts/release-notes.sh 0.0.9)
if [ "$got" = "- Older." ]; then ok; else fail "notes of the last version (it ends at the link lines): got '$got'"; fi
if out=$(cd "$repo" && scripts/release-notes.sh 0.2.0 2>&1); then fail "notes of a version that is not there succeeded"; elif grep -qF "no heading" <<< "$out"; then ok; else fail "notes: wrong message: $out"; fi

# ---- package-release.sh, on stand-in files ----
echo "== package-release.sh"
in=$tmp/in
mkdir -p "$in/ext/linux-x86_64" "$in/ext/darwin-arm64" "$in/ext/windows-x86_64" "$in/cli/linux-x86_64" "$in/cli/darwin-arm64" "$in/wheels" "$in/npm"
echo so > "$in/ext/linux-x86_64/forkdb.so"; echo dylib > "$in/ext/darwin-arm64/forkdb.dylib"; echo dll > "$in/ext/windows-x86_64/forkdb.dll"
echo linuxcli > "$in/cli/linux-x86_64/forkdb"; echo maccli > "$in/cli/darwin-arm64/forkdb"
for tag in manylinux_2_28_x86_64 macosx_11_0_arm64 win_amd64; do echo "wheel $tag" > "$in/wheels/forkdb-0.1.0-py3-none-$tag.whl"; done
echo npm > "$in/npm/sqlite-forkdb-0.1.0.tgz"
export SOURCE_DATE_EPOCH=1700000000
if "$root/scripts/package-release.sh" 0.1.0 "$in" "$tmp/out1" > "$tmp/pk1.log" 2>&1; then ok; else fail "package: $(cat "$tmp/pk1.log")"; fi
names=$(cd "$tmp/out1" && find . -maxdepth 1 -type f -printf '%f\n' | LC_ALL=C sort | tr '\n' ' ')
want_names='SHA256SUMS forkdb-0.1.0-py3-none-macosx_11_0_arm64.whl forkdb-0.1.0-py3-none-manylinux_2_28_x86_64.whl forkdb-0.1.0-py3-none-win_amd64.whl forkdb-cli-0.1.0-darwin-arm64.tar.gz forkdb-cli-0.1.0-linux-x86_64.tar.gz forkdb-ext-0.1.0-darwin-arm64.tar.gz forkdb-ext-0.1.0-linux-x86_64.tar.gz forkdb-ext-0.1.0-windows-x86_64.tar.gz sqlite-forkdb-0.1.0.tgz '
if [ "$names" = "$want_names" ]; then ok; else fail "package: files are '$names'"; fi
listing=$(tar -tvzf "$tmp/out1/forkdb-cli-0.1.0-linux-x86_64.tar.gz")
if grep -q '^-rwxr-xr-x 0/0 .* forkdb-cli-0.1.0-linux-x86_64/forkdb$' <<< "$listing"; then ok; else fail "package: the CLI is not 755 and owned by 0/0: $listing"; fi
if grep -q '^-rw-r--r-- 0/0 .* forkdb-cli-0.1.0-linux-x86_64/LICENSE-MIT$' <<< "$listing"; then ok; else fail "package: LICENSE-MIT missing or not 644: $listing"; fi
if tar -tzf "$tmp/out1/forkdb-ext-0.1.0-windows-x86_64.tar.gz" | grep -qx 'forkdb-ext-0.1.0-windows-x86_64/forkdb.dll'; then ok; else fail "package: forkdb.dll is not in the Windows extension archive"; fi
if [ "$(tar -xzOf "$tmp/out1/forkdb-ext-0.1.0-linux-x86_64.tar.gz" forkdb-ext-0.1.0-linux-x86_64/forkdb.so)" = so ]; then ok; else fail "package: the library inside the archive is not the input"; fi
if (cd "$tmp/out1" && sha256sum --check --strict SHA256SUMS > /dev/null); then ok; else fail "package: SHA256SUMS does not verify"; fi
if [ "$(wc -l < "$tmp/out1/SHA256SUMS")" -eq 9 ] && ! grep -q 'SHA256SUMS$' "$tmp/out1/SHA256SUMS"; then ok; else fail "package: SHA256SUMS must list the 9 files and not itself"; fi
# reproducible: the same inputs, a second time, in another place, give the same files
"$root/scripts/package-release.sh" 0.1.0 "$in" "$tmp/out2" > /dev/null 2>&1
if cmp -s "$tmp/out1/SHA256SUMS" "$tmp/out2/SHA256SUMS"; then ok; else fail "package: two runs on the same inputs differ"; fi
# every way an input can be wrong
pkfail() { # NAME MESSAGE
  local out
  if out=$("$root/scripts/package-release.sh" 0.1.0 "$in" "$tmp/outx" 2>&1); then fail "package: $1 succeeded"; elif grep -qF -- "$2" <<< "$out"; then ok; else fail "package: $1: wrong message: $out"; fi
}
mv "$in/cli/darwin-arm64/forkdb" "$tmp/held"
pkfail "a missing CLI binary" "missing input"
mv "$tmp/held" "$in/cli/darwin-arm64/forkdb"
echo again > "$in/wheels/forkdb-0.1.0-cp312-cp312-win_amd64.whl"
pkfail "two wheels for one platform" "expected exactly one wheel for win_amd64"
rm "$in/wheels/forkdb-0.1.0-cp312-cp312-win_amd64.whl"
mv "$in/wheels/forkdb-0.1.0-py3-none-macosx_11_0_arm64.whl" "$tmp/held"
pkfail "a missing wheel" "expected exactly one wheel for macosx_11_0_arm64"
mv "$tmp/held" "$in/wheels/forkdb-0.1.0-py3-none-macosx_11_0_arm64.whl"
mv "$in/npm/sqlite-forkdb-0.1.0.tgz" "$tmp/held"
pkfail "a missing npm tarball" "missing input"
mv "$tmp/held" "$in/npm/sqlite-forkdb-0.1.0.tgz"
if out=$("$root/scripts/package-release.sh" 0.2.0 "$in" "$tmp/outx" 2>&1); then fail "package: asking for another version succeeded"; elif grep -qF "expected exactly one wheel" <<< "$out"; then ok; else fail "package: another version: wrong message: $out"; fi

echo "$checks checks, $bad failures"
[ "$bad" -eq 0 ] || exit 1
echo "TOTAL FAILS=0"
