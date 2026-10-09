#!/usr/bin/env bash
# Download the pinned SQLite amalgamation into spike/vendor and verify its checksum.
# Needs curl, sha256sum and python3 (used to unzip; unzip itself is not required).
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
got=$(sha256sum "$dest/sqlite.zip" | cut -d' ' -f1)
if [ "$got" != "$SHA256" ]; then
  echo "checksum mismatch for $URL" >&2
  echo "  expected $SHA256" >&2
  echo "  got      $got" >&2
  rm -f "$dest/sqlite.zip"
  exit 1
fi
python3 - "$dest/sqlite.zip" "$dest" <<'PY'
import sys, zipfile
zipfile.ZipFile(sys.argv[1]).extractall(sys.argv[2])
PY
rm -f "$dest/sqlite.zip"
echo "fetched $VERSION_DIR into $dest"
