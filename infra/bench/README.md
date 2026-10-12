# infra/bench: forkdb on real SSD / NVMe (OpenTofu)

One throwaway EC2 host that runs the VFS benchmark (`vfs/vfs_bench`) on **local NVMe instance store** and on a **gp3 EBS volume**, so the numbers in `docs/VFS-SPIKE.md` (all from one WSL2 machine with a 4 ms fsync) can be compared with real disks. It also runs `scripts/test-vfs.sh quick` on a different kernel and filesystem. **Nothing here has been applied.** Plan and validate only, until you say go.

## What it creates

| Resource | Notes |
|---|---|
| `aws_instance` | `i4i.large` (2 vCPU, 16 GiB, 468 GB NVMe instance store) by default; `i3.large` allowed. Amazon Linux 2023. IMDSv2 required. Root gp3 20 GB and an extra gp3 20 GB (3000 IOPS, 125 MB/s) **at `/dev/sdf`**. Both volumes are deleted with the instance. |
| `aws_security_group` | **No inbound.** Outbound 443 and 80 only (package mirrors, the source download). |
| (data) AMI, default VPC, a default subnet in an AZ that offers the type | read-only |

No key pair, no instance role, no SSM. The source comes in by a **presigned S3 URL** (the repo is private, so the host cannot clone it), and the results leave on the **serial console** (`aws ec2 get-console-output`). The host runs the benchmark and then powers itself off. `instance_initiated_shutdown_behavior = terminate` makes that a termination, and a `shutdown -h +60` backstop is armed at boot, so a stuck script cannot leave it running past an hour.

## Cost (my estimate from memory of the on-demand price: confirm before applying)

| | per hour | this run (about 15 min: 5 to run, 10 held for the console) |
|---|---|---|
| `i4i.large` | about $0.17 | about $0.05 |
| `i3.large` | about $0.16 | about $0.04 |
| 2 x 20 GB gp3 | about $0.0022 | under $0.01 |
| public IPv4 | $0.005 | about $0.003 |

About **$0.05 for the run (the first run took 4.5 minutes and cost about $0.02), $4 a day if it were left running, which the backstop prevents.** An S3 object of about 150 KB and a state file of a few KB cost nothing measurable. Teardown leaves $0/day.

## One-time, by hand: the IAM policy (least privilege)

The agent's IAM user (`openclaw-ulrich`) cannot launch instances today (`ec2:RunInstances` is denied; checked with a free `--dry-run` on 2026-10-11). The agent has no IAM rights by design, so this is for you to run once with your own credentials. [iam-policy.json](iam-policy.json) allows only:

- **`RunInstances`** only with the tag `Project=forkdb-bench` on the instance and its volumes, only for `i4i.large` and `i3.large` (`StringEquals` with a list: an OR over the two values that fails closed if the key is missing; `ForAllValues` would pass when the key is absent), only with a security group that carries that tag, plus the public AMI, the subnet and the network interface (the provider does not tag the implicit NIC, so a tag condition on it would deny the launch; an untagged NIC is harmless).
- **`CreateTags`** only at creation (`RunInstances`, `CreateSecurityGroup`) and only with that tag.
- **Security group:** create one tagged `Project=forkdb-bench` in the VPC; authorize/revoke egress and delete **only security groups with that tag**.
- **`TerminateInstances`, `GetConsoleOutput`** only on instances with that tag.
- **Read-only `Describe*`** (these cannot be scoped to a resource).
- **S3:** list and get/put/delete **only under `forkdb/bench/` in the existing state bucket** (the state file and the source tarball).

No IAM, no SSM, no other EC2 actions, no other buckets or prefixes.

```sh
# with your own (admin) credentials:
ACCOUNT_ID=$(aws sts get-caller-identity --query Account --output text)
sed "s/ACCOUNT_ID/$ACCOUNT_ID/g" infra/bench/iam-policy.json > /tmp/forkdb-bench-policy.json
aws iam put-user-policy --user-name openclaw-ulrich --policy-name forkdb-bench --policy-document file:///tmp/forkdb-bench-policy.json
# to take it away again:
aws iam delete-user-policy --user-name openclaw-ulrich --policy-name forkdb-bench
```

I derived this list from what `aws_instance`, `aws_security_group` and the data sources call; I could not test it (the agent cannot run what is denied). If an apply hits an `UnauthorizedOperation`, the error names the exact action: I will report it and stop rather than ask for more.

## Run it (after the policy is attached, and only with your go)

```sh
export AWS_PROFILE=snooze-dev
infra/bench/package-source.sh            # tar the current commit, upload to s3://.../forkdb/bench/<sha>.tar.gz, write a presigned URL to ~/.config/forkdb-bench/tofu.env (mode 600)
set -a; . ~/.config/forkdb-bench/tofu.env; set +a
cd infra/bench && tofu init && tofu plan -out tfplan   # expect: 2 to add (security group, instance), 0 to change, 0 to destroy
tofu apply tfplan
# the host runs for about 5 minutes, prints FORKDB-BENCH-DONE and then stays up for 10 more so the console can be fetched.
# Save the console while it runs (timestamped copies, never overwritten; the console is empty after termination):
infra/bench/watch-console.sh "$(tofu output -raw instance_id)" /tmp/forkdb-bench-console
grep -aE 'FORKDB-BENCH-DONE|^\\[(nvme|ebs)\\] R ' /tmp/forkdb-bench-console/console-final.txt | head
tofu destroy                              # the instance has already terminated itself; this removes the security group and state
```

The `[nvme]` and `[ebs]` lines are the benchmark's `R <rep> <config> <metric> <value>` lines, the same format as `vfs/results/bench-3-reps.txt`, so the same aggregation applies. Expected: the `disk write4k+fsync` floor drops from 4 ms to tens of microseconds on the instance store (EBS is around a millisecond), and the commit ratios in `docs/VFS-SPIKE.md` move with it.

## What is not verified

- **Console capture.** The script writes to `/dev/console`, which is meant to land in the serial console buffer that `get-console-output` returns (it keeps the last 64 KB). It also writes `/var/log/forkdb-bench.log`, but with no inbound access and no key pair that file is unreadable. If the console shows boot messages but not the benchmark, the first run is the test and I would switch to the S3 route (give the instance a role that can put one object under `forkdb/bench/`). The 64 KB limit is far above the benchmark's output (a few KB).
- **The IAM list** (above).
- **The bootstrap** was rendered with `tofu` and checked with `bash -n` and shellcheck, not executed. It needs the AL2023 packages `gcc`, `libasan`, `libubsan`; if a package name is wrong the run prints `WARN: dnf install failed` and the benchmark (which needs only `gcc`) can still run, but the sanitizer tests would not.
- **Numbers on a different filesystem.** The WSL2 runs were ext4 on a virtual disk; here both disks are formatted ext4 with default options, so the filesystem matches and the disk differs.
- A single instance of one type in one AZ, three repetitions: the same limits as the WSL2 numbers.

## Teardown and safety

`tofu destroy` removes everything this module made. If the host is still running for any reason, `aws ec2 terminate-instances --instance-ids $(tofu output -raw instance_id)` stops the billing at once (the IAM policy allows it for tagged instances). State lives at `s3://benmckibben-openclaw-infra/forkdb/bench/terraform.tfstate` (encrypted, S3 lockfile); it holds nothing secret except the presigned URL, which expires after two hours and only reads one object.

Do not run `tofu apply` a second time without a `destroy`: the instance terminates itself, so the next plan would want to recreate it (and `user_data_replace_on_change` does the same if the script changes).

