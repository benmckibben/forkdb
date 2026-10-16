#!/usr/bin/env bash
# Print the CHANGELOG.md section of one version: what is under "## [X.Y.Z] - date", without the heading. The release workflow uses it as the text of the draft release.
#   scripts/release-notes.sh 0.1.0
# Fails if the heading is not there, or if there is nothing under it. The section ends at the next "## " heading or at the link lines a changelog may end with.
set -euo pipefail
version=${1:?usage: release-notes.sh VERSION}
here=$(cd "$(dirname "$0")/.." && pwd)
changelog=${CHANGELOG:-$here/CHANGELOG.md}

if ! awk -v v="$version" 'index($0, "## [" v "]") == 1 { found = 1 } END { exit !found }' "$changelog"; then
  echo "release-notes.sh: $changelog has no heading '## [$version]'" >&2
  exit 1
fi
notes=$(awk -v v="$version" '
  index($0, "## [" v "]") == 1 { on = 1; next }
  on && (/^## / || /^\[[^]]+\]: /) { exit }
  on { print }
' "$changelog" | sed -e '/./,$!d')
if [ -z "$notes" ]; then
  echo "release-notes.sh: the section '## [$version]' of $changelog is empty" >&2
  exit 1
fi
printf '%s\n' "$notes"
