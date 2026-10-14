const { ForkDB } = require('../index.js');
const fs = require('fs');
const path =require('path');

// Create a temporary database file for testing
const testDbPath = path.join(__dirname, 'test.fk');

// Clean up any existing test database
if (fs.existsSync(testDbPath)) {
  fs.unlinkSync(testDbPath);
}

console.log('Testing forkdb Node.js bindings...');

try {
  // Create and open database
  const db = new ForkDB(testDbPath);
  console.log('✓ ForkDB instance created');
  
  db.open();
  console.log('✓ Database opened');
  
  // Get initial info
  const mainInfo = db.info('main');
  console.log('✓ Got main branch info:', mainInfo.name, mainInfo.kind);
  
  // Create a fork
  db.fork('main', 'test-fork');
  console.log('✓ Created test-fork from main');
  
  // List branches
  const branches = db.list();
  console.log('✓ Listed branches:', branches.map(b => b.name).join(', '));
  
  // Get fork info
  const forkInfo = db.info('test-fork');
  console.log('✓ Got fork info:', forkInfo.name, 'forked from', forkInfo.originUid);
  
  // Close database
  db.close();
  console.log('✓ Database closed');
  
  // Clean up
  if (fs.existsSync(testDbPath)) {
    fs.unlinkSync(testDbPath);
    console.log('✓ Test database cleaned up');
  }
  
  console.log('\n🎉 All tests passed!');
  
} catch (error) {
  console.error('❌ Test failed:', error.message);
  console.error(error.stack);
  
  // Clean up on error
  if (fs.existsSync(testDbPath)) {
    fs.unlinkSync(testDbPath);
  }
  
  process.exit(1);
}