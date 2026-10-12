# One throwaway benchmark host. No inbound traffic, no key pair, no instance role: the source arrives by a presigned URL, and the results
# leave on the serial console (read them with `aws ec2 get-console-output`). The host shuts itself down when it is done.

data "aws_ami" "al2023" {
  most_recent = true
  owners      = ["amazon"]

  filter {
    name   = "name"
    values = ["al2023-ami-2023*-x86_64"]
  }
  filter {
    name   = "architecture"
    values = ["x86_64"]
  }
  filter {
    name   = "virtualization-type"
    values = ["hvm"]
  }
  filter {
    name   = "state"
    values = ["available"]
  }
}

data "aws_ec2_instance_type_offerings" "this" {
  location_type = "availability-zone"
  filter {
    name   = "instance-type"
    values = [var.instance_type]
  }
}

data "aws_vpc" "default" {
  default = true
}

# a default subnet in an availability zone that offers the instance type
data "aws_subnets" "offered" {
  filter {
    name   = "vpc-id"
    values = [data.aws_vpc.default.id]
  }
  filter {
    name   = "availability-zone"
    values = data.aws_ec2_instance_type_offerings.this.locations
  }
  filter {
    name   = "default-for-az"
    values = ["true"]
  }
}

resource "aws_security_group" "bench" {
  name_prefix = "forkdb-bench-"
  description = "forkdb benchmark host: no inbound, https and http out (package mirrors)"
  vpc_id      = data.aws_vpc.default.id

  egress {
    description = "https (dnf, S3 presigned URL, SQLite download)"
    from_port   = 443
    to_port     = 443
    protocol    = "tcp"
    cidr_blocks = ["0.0.0.0/0"]
  }
  egress {
    description = "http (package mirrors)"
    from_port   = 80
    to_port     = 80
    protocol    = "tcp"
    cidr_blocks = ["0.0.0.0/0"]
  }

  lifecycle {
    create_before_destroy = true
  }
}

locals {
  user_data = templatefile("${path.module}/templates/bootstrap.sh.tftpl", {
    source_url  = var.source_url
    source_sha  = var.source_sha
    reps        = var.reps
    bench_mode  = var.bench_mode
    run_tests   = var.run_tests ? "1" : "0"
    max_minutes = var.max_runtime_minutes
  })
}

resource "aws_instance" "bench" {
  ami                         = data.aws_ami.al2023.id
  instance_type               = var.instance_type
  subnet_id                   = sort(data.aws_subnets.offered.ids)[0]
  vpc_security_group_ids      = [aws_security_group.bench.id]
  associate_public_ip_address = true # needed to reach the package mirrors; nothing can reach in

  # the host powers itself off when done; this makes that a termination, so nothing keeps billing
  instance_initiated_shutdown_behavior = "terminate"

  metadata_options {
    http_endpoint               = "enabled"
    http_tokens                 = "required"
    http_put_response_hop_limit = 1
  }

  root_block_device {
    volume_type           = "gp3"
    volume_size           = 20
    delete_on_termination = true
  }

  # the network-attached disk to compare against the instance store; gp3 baseline is 3000 IOPS and 125 MB/s
  ebs_block_device {
    device_name           = "/dev/sdf"
    volume_type           = "gp3"
    volume_size           = var.ebs_gb
    iops                  = 3000
    throughput            = 125
    delete_on_termination = true
  }

  volume_tags = {
    Project = "forkdb-bench"
  }

  user_data                   = local.user_data
  user_data_replace_on_change = true

  tags = {
    Name = "forkdb-bench"
  }
}
