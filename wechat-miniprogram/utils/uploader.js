const MAX_VIDEO_BYTES = 50 * 1024 * 1024;

function pad(n) { return n < 10 ? '0' + n : '' + n; }

function monthDir(timeMs) {
  const d = new Date(timeMs);
  return d.getFullYear() + '-' + pad(d.getMonth() + 1);
}

function makeFileName(type, timeMs, ext) {
  const d = new Date(timeMs);
  const ts = '' + d.getFullYear() + pad(d.getMonth() + 1) + pad(d.getDate()) + '_' + pad(d.getHours()) + pad(d.getMinutes()) + pad(d.getSeconds());
  const suffix = ext || (type === 'video' ? 'mp4' : 'jpg');
  return (type === 'video' ? 'VID_' : 'IMG_') + ts + '.' + suffix;
}

function contentTypeFor(name) {
  const ext = name.split('.').pop().toLowerCase();
  const map = {
    jpg: 'image/jpeg', jpeg: 'image/jpeg', png: 'image/png', gif: 'image/gif',
    heic: 'image/heic', webp: 'image/webp',
    mp4: 'video/mp4', mov: 'video/quicktime', m4v: 'video/mp4', mkv: 'video/x-matroska'
  };
  return map[ext] || 'application/octet-stream';
}

// 缩略图约定：同目录同名，扩展名换成 .thumb.jpg
function thumbPathFor(path) {
  if (/\.[^./]+$/.test(path)) return path.replace(/\.[^./]+$/, '.thumb.jpg');
  return path + '.thumb.jpg';
}

// 本地压缩生成缩略图（上传时/首次浏览时调用）；不支持时返回 null
function makeImageThumb(src) {
  if (typeof wx === 'undefined' || !wx.compressImage) return Promise.resolve(null);
  return new Promise((resolve) => {
    wx.compressImage({
      src,
      quality: 60,
      compressedWidth: 480,
      success: (res) => resolve(res.tempFilePath),
      fail: () => resolve(null)
    });
  });
}

async function uniquePath(dav, dirPath, name) {
  let candidate = name;
  let n = 1;
  for (;;) {
    const found = await dav.propfind(dirPath + '/' + candidate, 0);
    if (!found || found.length === 0) return dirPath + '/' + candidate;
    const dot = name.lastIndexOf('.');
    const base = dot > 0 ? name.slice(0, dot) : name;
    const ext = dot > 0 ? name.slice(dot) : '';
    candidate = base + '-' + n + ext;
    n += 1;
  }
}

async function uploadFiles({ dav, files, onProgress }) {
  const total = files.length;
  let done = 0;
  for (let i = 0; i < total; i++) {
    const f = files[i];
    if (f.type === 'video' && f.size > MAX_VIDEO_BYTES) {
      throw Object.assign(new Error('视频超过 50MB 限制: ' + f.name), { code: 'TOO_LARGE', file: f });
    }
    const dir = monthDir(f.time);
    await dav.mkcol('/dav/backup/android/DCIM/' + dir);
    const path = await uniquePath(dav, '/dav/backup/android/DCIM/' + dir, f.name);
    await dav.upload(path, f.tempFilePath, (percent) => {
      if (onProgress) onProgress(done, total, percent, f.name);
    });
    // 图片顺带传一张压缩缩略图，月视图就不用下载原图
    if (f.type === 'image') {
      const thumb = await makeImageThumb(f.tempFilePath);
      if (thumb) {
        try { await dav.upload(thumbPathFor(path), thumb); } catch (e) { /* 缩略图失败不影响原图 */ }
      }
    }
    done += 1;
    if (onProgress) onProgress(done, total, 100, f.name);
  }
  return total;
}

module.exports = {
  MAX_VIDEO_BYTES, monthDir, makeFileName, contentTypeFor,
  thumbPathFor, makeImageThumb, uniquePath, uploadFiles
};
