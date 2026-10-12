#!/usr/bin/env bash
# Package the source at a commit, upload it under the bench prefix of the state bucket and write a presigned URL for it into a
# chmod 600 env file (the repo is private, so the benchmark host cannot clone it, and it has no credentials of its own).
#   infra/bench/package-source.sh [REF]       REF defaults to HEAD; run from a checkout of the commit you want measured
#   then:  set -a; . ~/.config/forkdb-bench/tofu.env; set +a; cd infra/bench && tofu init && tofu plan -out tfplan
set -euo pipefail

ref=${1:-HEAD}
bucket=${BENCH_BUCKET:-benmckibben-openclaw-infra}
profile=${AWS_PROFILE:-snooze-dev}
expires=${BENCH_URL_EXPIRES:-7200}
envfile=${BENCH_ENV_FILE:-$HOME/.config/forkdb-bench/tofu.env}

sha=$(git rev-parse --verify "$ref^{commit}")
tmp=$(mktemp -d)
trap 'rm -rf "$tmp"' EXIT
git archive --format=tar.gz -o "$tmp/src.tar.gz" "$sha"
key=forkdb/bench/$sha.tar.gz
aws s3 cp --profile "$profile" "$tmp/src.tar.gz" "s3://$bucket/$key" --only-show-errors
url=$(aws s3 presign --profile "$profile" "s3://$bucket/$key" --expires-in "$expires")

umask 077
mkdir -p "$(dirname "$envfile")"
{
  printf 'export TF_VAR_source_sha=%s\n' "$sha"
  printf 'export TF_VAR_source_url=%q\n' "$url"
} > "$envfile"
echo "uploaded s3://$bucket/$key ($(du -h "$tmp/src.tar.gz" | cut -f1)); URL valid for ${expires}s; wrote $envfile (mode 600)"

