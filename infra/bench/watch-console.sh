#!/usr/bin/env bash
# Save the benchmark host's serial console while it runs. Every poll is kept as its own timestamped file and nothing is ever overwritten:
# the console buffer is only refreshed every few minutes and AWS returns an EMPTY console once the instance has terminated.
#   infra/bench/watch-console.sh INSTANCE_ID [OUTDIR]     ends when the console shows FORKDB-BENCH-DONE or -FAILED, then saves a last copy
set -euo pipefail

id=${1:?usage: watch-console.sh INSTANCE_ID [OUTDIR]}
out=${2:-/tmp/forkdb-bench-console}
profile=${AWS_PROFILE:-snooze-dev}
region=${AWS_REGION:-us-east-1}
limit=${WATCH_LIMIT_S:-3000}
mkdir -p "$out"
start=$SECONDS
while [ $((SECONDS - start)) -lt "$limit" ]; do
  f=$out/console-$(date -u +%H%M%S).txt
  if aws ec2 get-console-output --profile "$profile" --region "$region" --instance-id "$id" --latest --output text > "$f" 2> "$f.err"; then
    [ -s "$f.err" ] || rm -f "$f.err"
  fi
  if grep -aq "FORKDB-BENCH-DONE\|FORKDB-BENCH-FAILED" "$f"; then
    cp "$f" "$out/console-final.txt"
    echo "done marker seen: $out/console-final.txt ($(wc -c < "$out/console-final.txt") bytes)"
    exit 0
  fi
  state=$(aws ec2 describe-instances --profile "$profile" --region "$region" --instance-ids "$id" --query "Reservations[0].Instances[0].State.Name" --output text 2>/dev/null || echo unknown)
  if [ "$state" = terminated ]; then echo "instance terminated before a done marker was seen; see the copies in $out"; exit 1; fi
  sleep 30
done
echo "gave up after ${limit}s; copies are in $out"
exit 1

