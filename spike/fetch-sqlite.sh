#!/usr/bin/env bash
# Download the pinned SQLite amalgamation into spike/vendor and verify its checksum.
# Needs curl, a SHA-256 tool (sha256sum, or shasum on macOS) and python (python3 or python; used to unzip, unzip itself is not required).
set -euo pipefail

VERSION_DIR=sqlite-amalgamation-3500000
URL=https://www.sqlite.org/2025/$VERSION_DIR.zip
SHA256=f196539ae448ecb304f05598d8614b125ace159a63a980dca691e066250d7fa2

here=$(cd "$(dirname "$0")" && pwd)
dest=$here/vendor
mkdir -p "$dest"

if [ -f "$dest/$VERSION_DIR/sqlite3.c" ]; then
  echo "already present: $dest/$VERSION_DIR"
  exit 0
fi

curl -fsSL -o "$dest/sqlite.zip" "$URL"
if command -v sha256sum > /dev/null; then got=$(sha256sum "$dest/sqlite.zip" | cut -d' ' -f1); else got=$(shasum -a 256 "$dest/sqlite.zip" | cut -d' ' -f1); fi
if [ "$got" != "$SHA256" ]; then
  echo "checksum mismatch for $URL" >&2
  echo "  expected $SHA256" >&2
  echo "  got      $got" >&2
  rm -f "$dest/sqlite.zip"
  exit 1
fi
py=$(command -v python3 || command -v python || { echo 'no python3 or python to unzip with' >&2; exit 1; })
"$py" - "$dest/sqlite.zip" "$dest" <<'PY'
import sys, zipfile
zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])
PY
rm -f "$dest/sqlite.zip"
echo "fetched $VERSION_DIR into $dest"
