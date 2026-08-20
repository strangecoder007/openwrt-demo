const assert = require('assert');
const auth = require('../utils/auth');

function testBase64() {
  assert.strictEqual(auth.base64Encode(''), '');
  assert.strictEqual(auth.base64Encode('hello'), 'aGVsbG8=');
  const expected = Buffer.from('backup:Backup@2026', 'utf8').toString('base64');
  assert.strictEqual(auth.base64Encode('backup:Backup@2026'), expected);
  assert.strictEqual(auth.makeAuthHeader('u', 'p'), 'Basic ' + Buffer.from('u:p').toString('base64'));
}

// Node 环境没有 wx，用一个内存 map 模拟 wx storage。
const store = {};
global.wx = {
  setStorageSync: (k, v) => { store[k] = v; },
  getStorageSync: (k) => (Object.prototype.hasOwnProperty.call(store, k) ? store[k] : ''),
  removeStorageSync: (k) => { delete store[k]; }
};

function testCredential() {
  auth.saveCredential({ baseUrl: 'https://cy.gcaiyy.xyz:34443', user: 'backup', pass: 's3cret' });
  assert.deepStrictEqual(auth.loadCredential(), { baseUrl: 'https://cy.gcaiyy.xyz:34443', user: 'backup', pass: 's3cret' });
  auth.clearCredential();
  assert.strictEqual(auth.loadCredential(), null);
}

function testSessionSeparate() {
  // 登录会话与“记住密码”是两个独立 storage key，互不影响。
  auth.saveSession({ baseUrl: 'https://x', user: 'u', pass: 'p' });
  assert.strictEqual(auth.loadCredential(), null);
  auth.saveCredential({ user: 'u', pass: 'p' });
  assert.strictEqual(auth.loadSession().user, 'u');
  auth.clearSession();
  assert.strictEqual(auth.loadSession(), null);
  assert.strictEqual(auth.loadCredential().user, 'u');
  auth.clearCredential();
  assert.strictEqual(auth.loadCredential(), null);
}

testBase64();
testCredential();
testSessionSeparate();
console.log('test-auth OK');
