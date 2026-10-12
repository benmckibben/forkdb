output "instance_id" {
  value = aws_instance.bench.id
}

output "availability_zone" {
  value = aws_instance.bench.availability_zone
}

output "ami" {
  value = data.aws_ami.al2023.id
}

output "read_results" {
  description = "Print the benchmark output once the host has shut down (the console buffer survives termination for about an hour)"
  value       = "aws ec2 get-console-output --profile ${var.aws_profile} --region ${var.aws_region} --instance-id ${aws_instance.bench.id} --latest --output text"
}
