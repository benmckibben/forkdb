const { spawn } = require('child_process');
const { promisify } = require('util');
const fs = require('fs');
const path = require('path');

// Wrapper for the forkdb CLI
class ForkDBCLIWrapper {
  constructor(dbPath) {
    this.dbPath = dbPath;
    // Find the forkdb CLI executable
    this.cliPath = this.findCLI();
  }

  findCLI() {
    // Look for the CLI in the expected locations
    const possiblePaths = [
      path.join(__dirname, '..', '..', '..', '..', 'build', 'forkdb'),
      path.join(__dirname, '..', '..', '..', '..', 'cli', 'forkdb'),
      'forkdb' // Try system PATH
    ];

    for (const cliPath of possiblePaths) {
      if (fs.existsSync(cliPath)) {
        return cliPath;
      }
    }

    throw new Error('forkdb CLI not found. Please build it first with ./scripts/build-step4.sh cli');
  }

  async executeCommand(args) {
    return new Promise((resolve, reject) => {
      const fullPath = path.resolve(this.dbPath);
      const allArgs = [fullPath, ...args];
      
      const child = spawn(this.cliPath, allArgs, {
        stdio: ['pipe', 'pipe', 'pipe']
      });

      let stdout = '';
      let stderr = '';

      child.stdout.on('data', (data) => {
        stdout += data.toString();
      });

      child.stderr.on('data', (data) => {
        stderr += data.toString();
      });

      child.on('close', (code) => {
        if (code === 0) {
          resolve(stdout.trim());
        } else {
          reject(new Error(`forkdb command failed with code ${code}: ${stderr}`));
        }
      });

      child.on('error', (err) => {
        reject(new Error(`Failed to spawn forkdb CLI: ${err.message}`));
      });
    });
  }

  async init(branchName = 'main') {
    await this.executeCommand(['init', '--name', branchName]);
    return this;
  }

  async fork(from, name) {
    await this.executeCommand(['fork', name, '--from', from]);
    return this;
  }

  async snapshot(from, name) {
    await this.executeCommand(['snapshot', name, '--from', from]);
    return this;
  }

  async discard(name) {
    await this.executeCommand(['discard', name]);
    return this;
  }

  async reset(name, to) {
    await this.executeCommand(['reset', name, to]);
    return this;
  }

  async info(ref = 'main') {
    const output = await this.executeCommand(['info', ref, '--json']);
    return JSON.parse(output);
  }

  async list(includeInternal = false) {
    const args = ['list'];
    if (includeInternal) {
      args.push('--all');
    }
    args.push('--json');
    
    const output = await this.executeCommand(args);
    return JSON.parse(output);
  }

  async promote(name, options = {}) {
    const args = ['promote', name];
    
    if (options.into) {
      args.push('--into', options.into);
    }
    
    if (options.dryRun) {
      args.push('--dry-run');
    }
    
    args.push('--json');
    
    const output = await this.executeCommand(args);
    return JSON.parse(output);
  }

  async undo(name) {
    const output = await this.executeCommand(['undo', name, '--json']);
    return JSON.parse(output);
  }

  async gc() {
    const output = await this.executeCommand(['gc', '--json']);
    return JSON.parse(output);
  }

  async compact(minBytes = 0) {
    const output = await this.executeCommand(['compact', '--min-bytes', minBytes.toString(), '--json']);
    return JSON.parse(output);
  }

  async sql(ref, sqlQuery, write = false) {
    const args = ['sql', ref];
    if (write) {
      args.push('--write');
    }
    args.push(sqlQuery);
    
    const output = await this.executeCommand(args);
    return output;
  }
}

module.exports = ForkDBCLIWrapper;