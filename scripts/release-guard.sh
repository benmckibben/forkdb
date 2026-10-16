#!/usr/bin/env bash
# The checks that decide whether a tag may become a release. The release workflow runs this first; every build and the draft release wait for it.
# On a tag run (REF_TYPE=tag):
#   - the five version strings agree (scripts/check-versions.sh);
#   - the tag is vMAJOR.MINOR.PATCH and is v<that version> (pre-release tags are refused: the package versions cannot spell them alike);
#   - the tagged commit is on the default branch;
#   - CHANGELOG.md has a heading "## [X.Y.Z] - YYYY-MM-DD" with something under it;
#   - the CI workflow has completed successfully on the tagged commit (a push to the default branch ran it). It needs the gh CLI, GH_TOKEN and GITHUB_REPOSITORY.
# On any other run it only prints the version, so a dry run from a branch gets the same builds without the tag checks.
# Inputs, from the workflow: REF_TYPE, REF_NAME, DEFAULT_BRANCH (default main), COMMIT_SHA (default HEAD), GITHUB_REPOSITORY, GITHUB_OUTPUT.
set -euo pipefail
cd "$(dirname "$0")/.."
ref_type=${REF_TYPE:-}
ref_name=${REF_NAME:-}
default_branch=${DEFAULT_BRANCH:-main}
sha=${COMMIT_SHA:-$(git rev-parse HEAD)}
fail() { echo "::error::$*"; exit 1; }

if ! versions=$(scripts/check-versions.sh); then
  echo "$versions"
  fail "the version is not the same in all five places"
fi
version=$(sed -n 's/^all five agree: //p' <<< "$versions")
[ -n "$version" ] || fail "could not read the version from scripts/check-versions.sh"
echo "version: $version"
if [ -n "${GITHUB_OUTPUT:-}" ]; then echo "version=$version" >> "$GITHUB_OUTPUT"; fi

if [ "$ref_type" != tag ]; then
  echo "::notice::not a tag run ($ref_type $ref_name): building $version for testing only; no release will be made"
  exit 0
fi

[[ "$ref_name" =~ ^v[0-9]+\.[0-9]+\.[0-9]+$ ]] \
  || fail "tag '$ref_name' is not of the form vMAJOR.MINOR.PATCH (pre-release tags are not supported)"
[ "$ref_name" = "v$version" ] \
  || fail "tag '$ref_name' does not match the version $version in the source (expected v$version)"

git merge-base --is-ancestor "$sha" "origin/$default_branch" \
  || fail "$sha is not reachable from $default_branch: a release tag must point at a commit on $default_branch"

escaped=${version//./\\.}
grep -Eq "^## \[$escaped\] - [0-9]{4}-[0-9]{2}-[0-9]{2}$" CHANGELOG.md \
  || fail "CHANGELOG.md has no heading '## [$version] - YYYY-MM-DD' (turn the Unreleased section into the version)"
scripts/release-notes.sh "$version" > /dev/null || fail "CHANGELOG.md has nothing under '## [$version]'"

repo=${GITHUB_REPOSITORY:?GITHUB_REPOSITORY is not set}
conclusion=$(gh api "repos/$repo/actions/workflows/ci.yml/runs?head_sha=$sha&event=push&per_page=50" \
  --jq '[.workflow_runs[] | select(.status == "completed")] | sort_by(.created_at) | last | .conclusion // "none"') \
  || fail "could not read the CI runs of $sha (the job needs the actions: read permission)"
[ "$conclusion" = success ] \
  || fail "the CI workflow has not completed successfully on $sha (its latest completed run: $conclusion). Wait for CI on $default_branch to finish, or re-run it, then run this workflow again"

echo "tag $ref_name matches version $version, is on $default_branch, has a changelog section, and CI is green on $sha"
