#!/usr/bin/env bash
# Turn the built pieces into the files of a release, and write SHA256SUMS for them. The release workflow runs it; it needs GNU tar and sha256sum (Linux).
#   scripts/package-release.sh VERSION INDIR OUTDIR
# INDIR holds what the build jobs made:
#   ext/<platform>/forkdb.<so|dylib|dll>   the loadable extension, for linux-x86_64, darwin-arm64, windows-x86_64
#   cli/<platform>/forkdb                  the command-line tool, for linux-x86_64 and darwin-arm64
#   wheels/*.whl                           one Python wheel per platform
#   npm/sqlite-forkdb-VERSION.tgz          the npm package
# OUTDIR gets exactly nine files and SHA256SUMS (9 + 1): three extension archives, two CLI archives, three wheels, the npm tarball. Anything missing, or any extra wheel, fails the run.
# The archives are reproducible: sorted names, owner 0, the time of the last commit (or SOURCE_DATE_EPOCH), gzip without a name or time. The compiled files inside them are not.
set -euo pipefail
version=${1:?usage: package-release.sh VERSION INDIR OUTDIR}
in=${2:?usage: package-release.sh VERSION INDIR OUTDIR}
out=${3:?usage: package-release.sh VERSION INDIR OUTDIR}
root=$(cd "$(dirname "$0")/.." && pwd)
fail() { echo "::error::package-release.sh: $*" >&2; exit 1; }
tar --version | grep -q 'GNU tar' || fail "needs GNU tar"

ext_platforms="linux-x86_64:so darwin-arm64:dylib windows-x86_64:dll"
cli_platforms="linux-x86_64 darwin-arm64"
wheel_tags="manylinux_2_28_x86_64 macosx_11_0_arm64 win_amd64"
epoch=${SOURCE_DATE_EPOCH:-$(git -C "$root" log -1 --format=%ct)}
docs="LICENSE-MIT LICENSE-APACHE README.md CHANGELOG.md"
for d in $docs; do [ -f "$root/$d" ] || fail "$d is missing from the repository"; done

rm -rf "$out"
mkdir -p "$out"
work=$(mktemp -d)
trap 'rm -rf "$work"' EXIT

archive() { # NAME FILE DESTNAME: NAME.tar.gz holding NAME/DESTNAME (mode 755) and the documents (644)
  local name=$1 file=$2 dest=$3
  [ -f "$file" ] || fail "missing input: $file"
  mkdir -p "$work/$name"
  install -m 755 "$file" "$work/$name/$dest"
  for d in $docs; do install -m 644 "$root/$d" "$work/$name/$d"; done
  tar --sort=name --owner=0 --group=0 --numeric-owner --mtime="@$epoch" -C "$work" -cf - "$name" | gzip -9n > "$out/$name.tar.gz"
}

for pe in $ext_platforms; do
  platform=${pe%%:*}; e=${pe#*:}
  archive "forkdb-ext-$version-$platform" "$in/ext/$platform/forkdb.$e" "forkdb.$e"
done
for platform in $cli_platforms; do
  archive "forkdb-cli-$version-$platform" "$in/cli/$platform/forkdb" forkdb
done
for tag in $wheel_tags; do
  matches=("$in"/wheels/forkdb-"$version"-*-"$tag".whl)
  [ "${#matches[@]}" -eq 1 ] && [ -f "${matches[0]}" ] || fail "expected exactly one wheel for $tag and version $version in $in/wheels, found: ${matches[*]}"
  cp "${matches[0]}" "$out/"
done
[ -f "$in/npm/sqlite-forkdb-$version.tgz" ] || fail "missing input: $in/npm/sqlite-forkdb-$version.tgz"
cp "$in/npm/sqlite-forkdb-$version.tgz" "$out/"

count=$(find "$out" -maxdepth 1 -type f | wc -l)
[ "$count" -eq 9 ] || fail "expected 9 files, made $count"
mapfile -t files < <(cd "$out" && find . -maxdepth 1 -type f -printf '%f\n' | LC_ALL=C sort) # listed before SHA256SUMS exists
(cd "$out" && sha256sum "${files[@]}" > SHA256SUMS)
(cd "$out" && sha256sum --check --strict SHA256SUMS)
echo "packaged $count files and SHA256SUMS in $out"
