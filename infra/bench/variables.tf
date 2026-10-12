variable "aws_region" {
  description = "Region (matches the snooze-dev profile)"
  type        = string
  default     = "us-east-1"
}

variable "aws_profile" {
  description = "AWS named profile"
  type        = string
  default     = "snooze-dev"
}

variable "instance_type" {
  description = "An instance type with local NVMe instance store. i4i.large: 2 vCPU, 16 GiB, 468 GB NVMe. i3.large is the older, cheaper alternative."
  type        = string
  default     = "i4i.large"
  validation {
    condition     = contains(["i4i.large", "i3.large"], var.instance_type)
    error_message = "instance_type must be i4i.large or i3.large (the IAM policy in iam-policy.json allows only these)."
  }
}

variable "ebs_gb" {
  description = "Size of the extra gp3 volume (GB). The benchmark database is about 34 MB; 20 is plenty."
  type        = number
  default     = 20
}

variable "source_url" {
  description = "Presigned https URL of the source tarball (see package-source.sh). The repo is private, so the instance cannot clone it. Sensitive: the URL carries a signature."
  type        = string
  sensitive   = true
}

variable "source_sha" {
  description = "The commit the tarball was made from; printed in the results so they can be tied to code."
  type        = string
}

variable "reps" {
  description = "Repetitions of the benchmark on each disk"
  type        = number
  default     = 3
}

variable "run_tests" {
  description = "Also run scripts/test-vfs.sh quick on the instance (a different kernel and filesystem than the WSL2 runs)"
  type        = bool
  default     = true
}

variable "max_runtime_minutes" {
  description = "Backstop: the instance shuts itself down (and, with instance-initiated shutdown set to terminate, is deleted) after this long, whatever the script is doing"
  type        = number
  default     = 60
}

variable "extra_tags" {
  description = "Extra tags on every resource"
  type        = map(string)
  default     = {}
}
