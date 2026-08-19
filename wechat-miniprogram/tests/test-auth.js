const assert = require('assert');
const auth = require('../utils/auth');

function testBase64() {
  assert.strictEqual(auth.base64Encode(''), '');
  assert.strictEqual(auth.base64Encode('hello'), 'aGVsbG8=');
  const expected = Buffer.from('backup:Backup@2026', 'utf8').toString('base64');
  assert.strictEqual(auth.base64Encode('backup:Backup@2026'), expected);
  assert.strictEqual(auth.makeAuthHeader('u', 'p'), 'Basic ' + Buffer.from('u:p').toString('base64'));
}

testBase64();
console.log('test-auth OK');
