# Remote state in S3 (SSE-encrypted, native S3 lockfile; OpenTofu >= 1.10), in the same bucket as the other projects, under its own prefix.
# Backend blocks cannot use variables; override per environment with
#   tofu init -reconfigure -backend-config="bucket=..." -backend-config="key=..."
terraform {
  backend "s3" {
    bucket       = "benmckibben-openclaw-infra"
    key          = "forkdb/bench/terraform.tfstate"
    region       = "us-east-1"
    profile      = "snooze-dev"
    encrypt      = true
    use_lockfile = true
  }
}
