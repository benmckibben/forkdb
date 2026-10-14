const { ForkDB } = require('../index.js');
const fs = require('fs');
const path = require('path');

// Create a temporary database file for testing
const testDbPath = path.join(__dirname, 'test-cli.fk');

// Clean up any existing test database
if (fs.existsSync(testDbPath)) {
  fs.unlinkSync(testDbPath);
}

async function runTests() {
  console.log('Testing forkdb CLI wrapper...');
  
  try {
    // Create ForkDB instance
    const db = new ForkDB(testDbPath);
    console.log('✓ ForkDB instance created');
    
    // Initialize database
    await db.init();
    console.log('✓ Database initialized');
    
    // Get initial info
    const mainInfo = await db.info('main');
    console.log('✓ Got main branch info:', mainInfo.name, mainInfo.kind);
    
    // Create a fork
    await db.fork('main', 'test-fork');
    console.log('✓ Created test-fork from main');
    
    // List branches
    const branches = await db.list();
    console.log('✓ Listed branches:', branches.map(b => b.name).join(', '));
    
    // Get fork info
    const forkInfo = await db.info('test-fork');
    console.log('✓ Got fork info:', forkInfo.name, 'forked from', forkInfo.originUid);
    
    // Clean up
    if (fs.existsSync(testDbPath)) {
      fs.unlinkSync(testDbPath);
      console.log('✓ Test database cleaned up');
    }
    
    console.log('\n🎉 All CLI wrapper tests passed!');
    
  } catch (error) {
    console.error('❌ Test failed:', error.message);
    console.error(error.stack);
    
    // Clean up on error
    if (fs.existsSync(testDbPath)) {
      fs.unlinkSync(testDbPath);
    }
    
    process.exit(1);
  }
}

runTests();