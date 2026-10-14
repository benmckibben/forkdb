const ForkDBCLIWrapper = require('./lib/forkdb-cli-wrapper.js');

/**
 * forkdb - A branchable, snapshot-based SQLite page store
 * 
 * This is a JavaScript wrapper around the forkdb CLI tool.
 * 
 * @example
 * const forkdb = require('forkdb');
 * const db = new forkdb.ForkDB('/path/to/database.fk');
 * await db.init();
 * await db.fork('main', 'feature-branch');
 * // ... do work ...
 * await db.promote('feature-branch');
 */

class ForkDB {
  /**
   * Create a new ForkDB instance
   * @param {string} dbPath - Path to the database file
   */
  constructor(dbPath) {
    this.wrapper = new ForkDBCLIWrapper(dbPath);
  }

  /**
   * Initialize a new database
   * @param {string} branchName - Name of the initial branch (default: 'main')
   * @returns {Promise<ForkDB>} This instance
   */
  async init(branchName = 'main') {
    await this.wrapper.init(branchName);
    return this;
  }

  /**
   * Create a new branch from an existing branch
   * @param {string} from - Source branch name
   * @param {string} name - New branch name
   * @returns {Promise<ForkDB>} This instance
   */
  async fork(from, name) {
    await this.wrapper.fork(from, name);
    return this;
  }

  /**
   * Create a read-only snapshot from a branch
   * @param {string} from - Source branch name
   * @param {string} name - Snapshot name
   * @returns {Promise<ForkDB>} This instance
   */
  async snapshot(from, name) {
    await this.wrapper.snapshot(from, name);
    return this;
  }

  /**
   * Remove a branch or snapshot
   * @param {string} name - Branch or snapshot name to discard
   * @returns {Promise<ForkDB>} This instance
   */
  async discard(name) {
    await this.wrapper.discard(name);
    return this;
  }

  /**
   * Reset a branch to match another branch's content
   * @param {string} name - Branch to reset
   * @param {string} to - Source branch to reset to
   * @returns {Promise<ForkDB>} This instance
   */
  async reset(name, to) {
    await this.wrapper.reset(name, to);
    return this;
  }

  /**
   * Get information about a branch
   * @param {string} ref - Branch reference (default: 'main')
   * @returns {Promise<Object>} Branch information
   */
  async info(ref = 'main') {
    return await this.wrapper.info(ref);
  }

  /**
   * List all branches
   * @param {boolean} includeInternal - Include internal branches (default: false)
   * @returns {Promise<Array>} Array of branch information
   */
  async list(includeInternal = false) {
    return await this.wrapper.list(includeInternal);
  }

  /**
   * Promote a branch to its target
   * @param {string} name - Branch name to promote
   * @param {Object} options - Promotion options
   * @param {string} options.into - Target branch (default: branch's origin)
   * @param {boolean} options.dryRun - Run without making changes (default: false)
   * @returns {Promise<Object>} Promotion result
   */
  async promote(name, options = {}) {
    return await this.wrapper.promote(name, options);
  }

  /**
   * Undo the last promotion into a branch
   * @param {string} name - Branch name to undo
   * @returns {Promise<Object>} Undo result
   */
  async undo(name) {
    return await this.wrapper.undo(name);
  }

  /**
   * Perform garbage collection
   * @returns {Promise<Object>} GC report
   */
  async gc() {
    return await this.wrapper.gc();
  }

  /**
   * Compact the database if needed
   * @param {number} minBytes - Minimum bytes threshold (default: 0)
   * @returns {Promise<Object>} Compaction result
   */
  async compact(minBytes = 0) {
    return await this.wrapper.compact(minBytes);
  }

  /**
   * Execute SQL on a branch
   * @param {string} ref - Branch reference
   * @param {string} sqlQuery - SQL query to execute
   * @param {boolean} write - Allow write operations (default: false)
   * @returns {Promise<string>} Query result
   */
  async sql(ref, sqlQuery, write = false) {
    return await this.wrapper.sql(ref, sqlQuery, write);
  }
}

/**
 * Convenience function to create and initialize a database
 * @param {string} dbPath - Path to the database file
 * @param {string} branchName - Initial branch name (default: 'main')
 * @returns {Promise<ForkDB>} Initialized ForkDB instance
 */
async function open(dbPath, branchName = 'main') {
  const db = new ForkDB(dbPath);
  await db.init(branchName);
  return db;
}

module.exports = {
  ForkDB,
  open
};