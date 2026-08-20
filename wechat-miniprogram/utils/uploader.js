const MAX_VIDEO_BYTES = 500 * 1024 * 1024;

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

// 预览图约定：同目录同名，扩展名换成 .preview.jpg（1280px，供全屏预览）
function previewPathFor(path) {
  if (/\.[^./]+$/.test(path)) return path.replace(/\.[^./]+$/, '.preview.jpg');
  return path + '.preview.jpg';
}

// 视频压缩版约定：同目录同名，扩展名换成 .preview.mp4（供点视频时优先播放）
function previewVideoPathFor(path) {
  if (/\.[^./]+$/.test(path)) return path.replace(/\.[^./]+$/, '.preview.mp4');
  return path + '.preview.mp4';
}

// 缩略图/预览图/视频压缩版（含历史 bug 产生的 .thumb.thumb*.jpg 链）不参与列表与计数
function isThumbPath(path) {
  return /\.thumb\.jpg$/i.test(path);
}

function isPreviewPath(path) {
  return /\.preview\.(jpg|mp4)$/i.test(path);
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

// 本地压缩生成预览图（1280px，比原图小一个量级，全屏观感接近原图）
function makeImagePreview(src) {
  if (typeof wx === 'undefined' || !wx.compressImage) return Promise.resolve(null);
  return new Promise((resolve) => {
    wx.compressImage({
      src,
      quality: 75,
      compressedWidth: 1280,
      success: (res) => resolve(res.tempFilePath),
      fail: () => resolve(null)
    });
  });
}

// 本地压缩生成视频压缩版（medium 质量，体积小、手机上耗时可接受）；
// 不支持 wx.compressVideo（基础库 < 2.11.0）时返回 null，静默跳过
function makeVideoPreview(src) {
  if (typeof wx === 'undefined' || !wx.compressVideo) return Promise.resolve(null);
  return new Promise((resolve) => {
    wx.compressVideo({
      src,
      quality: 'medium',
      success: (res) => resolve(res.tempFilePath || null),
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

async function uploadFiles({ dav, files, onProgress, makeThumb = makeImageThumb, makePreview = makeImagePreview, compressVideo = makeVideoPreview }) {
  const total = files.length;
  let done = 0;
  for (let i = 0; i < total; i++) {
    const f = files[i];
    if (f.type === 'video' && f.size > MAX_VIDEO_BYTES) {
      throw Object.assign(new Error('视频超过 500MB 限制: ' + f.name), { code: 'TOO_LARGE', file: f });
    }
    const dir = monthDir(f.time);
    await dav.mkcol('/dav/backup/android/DCIM/' + dir);
    const candidate = await uniquePath(dav, '/dav/backup/android/DCIM/' + dir, f.name);
    // 服务端会原子分配唯一名（并发同名可能再改成 -N 后缀），以返回的最终
    // 路径为准；旧版桥返回 null 时回退到客户端查重得到的候选名。
    const path = (await dav.upload(candidate, f.tempFilePath, (percent) => {
      if (onProgress) onProgress(done, total, percent, f.name, done);
    })) || candidate;
    // 图片顺带传一张压缩缩略图，月视图就不用下载原图
    if (f.type === 'image') {
      const thumb = await makeThumb(f.tempFilePath);
      if (thumb) {
        try { await dav.upload(thumbPathFor(path), thumb); } catch (e) { /* 缩略图失败不影响原图 */ }
      }
      // 再传一张 1280px 预览图，全屏预览不用下载原图
      const preview = await makePreview(f.tempFilePath);
      if (preview) {
        try { await dav.upload(previewPathFor(path), preview); } catch (e) { /* 预览图失败不影响原图 */ }
      }
    } else if (f.type === 'video') {
      // 本地压缩一个 .preview.mp4，点视频时优先播放它，流量小很多；
      // 压缩期间发一个 0% 进度回调，避免进度条卡在上一个文件的 100%
      if (onProgress) onProgress(done, total, 0, f.name, done);
      const preview = await compressVideo(f.tempFilePath);
      if (preview) {
        try { await dav.upload(previewVideoPathFor(path), preview); } catch (e) { /* 压缩版失败不影响原视频 */ }
      }
      // 视频用 chooseMedia 自带的封面做缩略图（手机端无法截帧）
      if (f.thumbTempFilePath) {
        try { await dav.upload(thumbPathFor(path), f.thumbTempFilePath); } catch (e) { /* 封面失败不影响原视频 */ }
      }
    }
    done += 1;
    if (onProgress) onProgress(done, total, 100, f.name, done - 1);
  }
  return total;
}

module.exports = {
  MAX_VIDEO_BYTES, monthDir, makeFileName, contentTypeFor,
  thumbPathFor, previewPathFor, previewVideoPathFor, isThumbPath, isPreviewPath,
  makeImageThumb, makeImagePreview, makeVideoPreview, uniquePath, uploadFiles
};
