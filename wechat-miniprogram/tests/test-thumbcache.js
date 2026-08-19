const assert = require('assert');
const fs = require('fs');
const os = require('os');
const path = require('path');
const { cacheKeyForPath, selectEvictions, createThumbCache } = require('../utils/thumbcache');

// node 真实文件系统适配，对齐小程序 FileSystemManager 的同步接口
const nodeFs = {
  exists(p) { try { fs.accessSync(p); return true; } catch (e) { return false; } },
  mkdir(p) { fs.mkdirSync(p, { recursive: true }); },
  copy(src, dst) { fs.copyFileSync(src, dst); },
  unlink(p) { fs.unlinkSync(p); },
  readJSON(p) { try { return JSON.parse(fs.readFileSync(p, 'utf8')); } catch (e) { return null; } },
  writeJSON(p, obj) { fs.writeFileSync(p, JSON.stringify(obj), 'utf8'); },
  size(p) { return fs.statSync(p).size; }
};

function tmpDir() {
  return fs.mkdtempSync(path.join(os.tmpdir(), 'thumbcache-'));
}

async function main() {
  // 缓存键：同一路径稳定、不同路径不冲突、只含安全字符
  const a = cacheKeyForPath('/dav/backup/android/DCIM/2026-08/IMG_20260819_123456.jpg');
  const b = cacheKeyForPath('/dav/backup/android/DCIM/2026-08/IMG_20260819_123457.jpg');
  assert.notStrictEqual(a, b);
  assert.ok(/^[A-Za-z0-9._-]+$/.test(a), 'key 只含安全字符: ' + a);
  assert.strictEqual(cacheKeyForPath('/dav/测试 图.jpg'), cacheKeyForPath('/dav/测试 图.jpg'));
  // 路径里的 %xx 与字面下划线不碰撞
  assert.notStrictEqual(cacheKeyForPath('/dav/_41.jpg'), cacheKeyForPath('/dav/%41.jpg'));

  // LRU 选择：只删最旧、直到数量与字节都达标
  assert.deepStrictEqual(selectEvictions([], { maxBytes: 100, maxFiles: 10 }), []);
  const three = [
    { key: 'a', size: 1, time: 1 },
    { key: 'b', size: 1, time: 2 },
    { key: 'c', size: 1, time: 3 }
  ];
  assert.deepStrictEqual(selectEvictions(three, { maxBytes: 100, maxFiles: 2 }), ['a']);
  const big = [
    { key: 'old', size: 80, time: 1 },
    { key: 'mid', size: 80, time: 2 },
    { key: 'new', size: 80, time: 3 }
  ];
  assert.deepStrictEqual(selectEvictions(big, { maxBytes: 200, maxFiles: 99 }), ['old']);
  assert.deepStrictEqual(selectEvictions(big, { maxBytes: 100, maxFiles: 99 }), ['old', 'mid']);
  // 单个文件本身就超限时不再删（避免把缓存清空）
  assert.deepStrictEqual(selectEvictions([{ key: 'only', size: 500, time: 1 }], { maxBytes: 100, maxFiles: 99 }), []);

  // 真实文件系统的存取、持久化、删除
  const dir = tmpDir();
  try {
    const src = path.join(dir, 'src.jpg');
    fs.writeFileSync(src, 'THUMB-BYTES');
    const cache = createThumbCache(nodeFs, dir, { maxBytes: 1000, maxFiles: 10 });
    cache.put('k1', src);
    const hit = cache.get('k1');
    assert.ok(hit, 'put 后应命中');
    assert.strictEqual(fs.readFileSync(hit, 'utf8'), 'THUMB-BYTES');
    assert.strictEqual(cache.get('nope'), null);

    // manifest 落盘：重建实例后仍命中
    const cache2 = createThumbCache(nodeFs, dir, { maxBytes: 1000, maxFiles: 10 });
    assert.ok(cache2.get('k1'), '重建后应命中');
    cache2.remove('k1');
    assert.strictEqual(cache2.get('k1'), null);

    // 超限自动清理：maxFiles=2，放 3 个 → 最旧的被淘汰
    const dir2 = tmpDir();
    try {
      const c3 = createThumbCache(nodeFs, dir2, { maxBytes: 1 << 20, maxFiles: 2 });
      for (const k of ['x1', 'x2', 'x3']) {
        const s = path.join(dir2, k + '.src');
        fs.writeFileSync(s, 'DATA-' + k);
        c3.put(k, s);
      }
      assert.strictEqual(c3.get('x1'), null, '超限后最旧的 x1 应被淘汰');
      assert.ok(c3.get('x2'), 'x2 应保留');
      assert.ok(c3.get('x3'), 'x3 应保留');
    } finally {
      fs.rmSync(dir2, { recursive: true, force: true });
    }
  } finally {
    fs.rmSync(dir, { recursive: true, force: true });
  }

  console.log('test-thumbcache OK');
}

main();
