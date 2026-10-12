provider "aws" {
  region  = var.aws_region
  profile = var.aws_profile

  default_tags {
    tags = merge({
      Project   = "forkdb-bench"
      ManagedBy = "opentofu"
    }, var.extra_tags)
  }
}
