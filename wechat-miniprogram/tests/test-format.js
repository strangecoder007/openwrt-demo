const assert = require('assert');
const { formatBytes } = require('../utils/format');

function main() {
  // 手工推导的字面期望：边界、小数位、超大值
  assert.strictEqual(formatBytes(0), '0 B');
  assert.strictEqual(formatBytes(512), '512 B');
  assert.strictEqual(formatBytes(1024), '1 KB');
  assert.strictEqual(formatBytes(1536), '1.5 KB');
  assert.strictEqual(formatBytes(1024 * 1024), '1 MB');
  assert.strictEqual(formatBytes(5 * 1024 * 1024), '5 MB');
  assert.strictEqual(formatBytes(12.4 * 1024 * 1024), '12.4 MB');
  assert.strictEqual(formatBytes(1024 * 1024 * 1024), '1 GB');
  assert.strictEqual(formatBytes(3.7 * 1024 * 1024 * 1024), '3.7 GB');
  assert.strictEqual(formatBytes(2048 * 1024 * 1024 * 1024), '2 TB');
  // 非法输入按 0 处理，不抛异常
  assert.strictEqual(formatBytes(-1), '0 B');
  assert.strictEqual(formatBytes(NaN), '0 B');
  assert.strictEqual(formatBytes(undefined), '0 B');
  console.log('test-format OK');
}

main();
