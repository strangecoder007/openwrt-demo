// 缩略图本地持久缓存：把服务端 .thumb.jpg 落到 USER_DATA_PATH，
// 命中后零网络请求；超限按 LRU 淘汰最旧项。

// WebDAV path → 安全文件名。先把字面 % 转成 %25，再整体 URI 编码并把 % 换成
// 下划线，保证与路径里天然存在的下划线不碰撞、且只有 [A-Za-z0-9._-]。
function cacheKeyForPath(path) {
  return encodeURIComponent(path.replace(/%/g, '%25')).replace(/%/g, '_');
}

// 给定 manifest 条目（含 size/time），返回应淘汰的 key（最旧优先），
// 直到总字节 ≤ maxBytes 且数量 ≤ maxFiles；绝不删空缓存。
function selectEvictions(entries, { maxBytes, maxFiles }) {
  if (!Array.isArray(entries) || entries.length === 0) return [];
  const sorted = entries.slice().sort((x, y) => x.time - y.time);
  let bytes = sorted.reduce((s, e) => s + (e.size || 0), 0);
  let remaining = sorted.length;
  const evict = [];
  for (const e of sorted) {
    if (remaining <= maxFiles && bytes <= maxBytes) break;
    if (remaining === 1) break;
    evict.push(e.key);
    bytes -= e.size || 0;
    remaining -= 1;
  }
  return evict;
}

function createThumbCache(fsLike, dir, opts = {}) {
  const maxBytes = opts.maxBytes || 30 * 1024 * 1024;
  const maxFiles = opts.maxFiles || 500;
  const manifestPath = dir + '/manifest.json';
  fsLike.mkdir(dir);

  function loadManifest() {
    const m = fsLike.readJSON(manifestPath);
    return m && typeof m === 'object' ? m : {};
  }

  function saveManifest(m) {
    fsLike.writeJSON(manifestPath, m);
  }

  function filePath(key) {
    return dir + '/' + key;
  }

  function cleanup(m) {
    const entries = Object.keys(m).map((key) => ({
      key,
      size: (m[key] && m[key].size) || 0,
      time: (m[key] && m[key].time) || 0
    }));
    const evict = selectEvictions(entries, { maxBytes, maxFiles });
    for (const key of evict) {
      try { fsLike.unlink(filePath(key)); } catch (e) { /* 文件可能已丢 */ }
      delete m[key];
    }
    if (evict.length) saveManifest(m);
  }

  return {
    filePath,
    get(key) {
      const m = loadManifest();
      const rec = m[key];
      if (!rec || !fsLike.exists(filePath(key))) return null;
      rec.time = Date.now();
      saveManifest(m);
      return filePath(key);
    },
    put(key, srcPath) {
      const m = loadManifest();
      fsLike.copy(srcPath, filePath(key));
      m[key] = { size: fsLike.size(filePath(key)), time: Date.now() };
      saveManifest(m);
      cleanup(m);
      return filePath(key);
    },
    remove(key) {
      const m = loadManifest();
      if (m[key]) delete m[key];
      if (fsLike.exists(filePath(key))) fsLike.unlink(filePath(key));
      saveManifest(m);
    }
  };
}

// 小程序 FileSystemManager 同步接口适配（wx.getFileSystemManager 基础库 1.9.9+）
function createWxFs() {
  const fsm = wx.getFileSystemManager();
  return {
    exists(p) { try { fsm.accessSync(p); return true; } catch (e) { return false; } },
    // 目录已存在属正常（部分基础库/开发者工具对 recursive mkdirSync 已存在目录
    // 仍抛 "file already exists"），先探测再创建，保证幂等
    mkdir(p) {
      try { fsm.accessSync(p); return; } catch (e) { /* 不存在才创建 */ }
      fsm.mkdirSync(p, true);
    },
    copy(src, dst) { fsm.copyFileSync(src, dst); },
    unlink(p) { fsm.unlinkSync(p); },
    readJSON(p) { try { return JSON.parse(fsm.readFileSync(p, 'utf8')); } catch (e) { return null; } },
    writeJSON(p, obj) { fsm.writeFileSync(p, JSON.stringify(obj), 'utf8'); },
    size(p) { return fsm.statSync(p).size; }
  };
}

module.exports = { cacheKeyForPath, selectEvictions, createThumbCache, createWxFs };
