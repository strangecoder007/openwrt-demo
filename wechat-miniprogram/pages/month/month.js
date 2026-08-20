const { getDav, authHeader } = require('../../utils/session');
const { thumbPathFor, previewPathFor, previewVideoPathFor, isThumbPath, isPreviewPath, makeImageThumb, makeImagePreview } = require('../../utils/uploader');
const { cacheKeyForPath, createThumbCache, createWxFs } = require('../../utils/thumbcache');
const { formatBytes } = require('../../utils/format');

// 缩略图/预览图共用本地持久缓存（命中后零网络请求；超限按 LRU 淘汰）
const thumbCache = createThumbCache(createWxFs(), wx.env.USER_DATA_PATH + '/thumbcache', { maxBytes: 100 * 1024 * 1024, maxFiles: 500 });

// 从文件名里的 YYYYMMDD 或服务器 lastModified 提取日期；都没有则归"未分类"
function dayInfo(name, lastModified) {
  const m = name.match(/(\d{8})/);
  if (m) {
    const mo = parseInt(m[1].slice(4, 6), 10);
    const d = parseInt(m[1].slice(6, 8), 10);
    return { key: m[1], label: mo + '月' + d + '日' };
  }
  if (lastModified) {
    const t = new Date(lastModified);
    if (!isNaN(t.getTime())) {
      const y = t.getFullYear();
      const mo = t.getMonth() + 1;
      const d = t.getDate();
      const key = '' + y + (mo < 10 ? '0' : '') + mo + (d < 10 ? '0' : '') + d;
      return { key, label: mo + '月' + d + '日' };
    }
  }
  return { key: '00000000', label: '未分类' };
}

Page({
  data: { dirPath: '', monthName: '', days: [], editing: false, selected: {}, selectedCount: 0, videoSrc: '', playingName: '' },
  onLoad(q) {
    const dirPath = decodeURIComponent(q.path);
    const monthName = dirPath.split('/').filter(Boolean).pop() || '';
    this.setData({ dirPath, monthName });
  },
  onShow() { this.loadFiles(); },
  async loadFiles() {
    if (this._loading) return;
    this._loading = true;
    try {
      const dav = getDav();
      const items = await dav.propfind(this.data.dirPath, 1) || [];
      const files = items
        .filter((f) => !f.isDir && !isThumbPath(f.href) && !isPreviewPath(f.href))
        .map((f) => {
          const name = f.href.split('/').filter(Boolean).pop();
          const info = dayInfo(name, f.lastModified);
          const isVideo = f.contentType.startsWith('video/');
          return {
            name,
            size: f.contentLength,
            type: isVideo ? 'video' : 'image',
            path: f.href,
            previewVideoPath: isVideo ? previewVideoPathFor(f.href) : '',
            // 服务端 ls 顺带 stat 派生兄弟，省掉每张图一次 depth=0 探测；
            // 旧版桥没有这些字段（undefined）时走原探测逻辑兼容
            hasThumb: !!f.hasThumb,
            hasPreview: !!f.hasPreview,
            hasPreviewVideo: !!f.hasPreviewVideo,
            lastModified: f.lastModified,
            dayKey: info.key,
            dayLabel: info.label,
            thumb: '',
            status: 'pending'
          };
        })
        .sort((a, b) => (a.name < b.name ? 1 : -1));
      this.allFiles = files;
      this.setData({ days: this.groupByDay(files), selected: {}, selectedCount: 0 });
      await this.loadThumbs(files);
    } catch (e) {
      wx.showToast({ title: e.message || '加载失败', icon: 'none' });
    } finally {
      this._loading = false;
    }
  },
  onPullDownRefresh() {
    this.loadFiles().catch(() => {}).then(() => wx.stopPullDownRefresh());
  },
  groupByDay(files) {
    const groups = {};
    for (const f of files) (groups[f.dayKey] = groups[f.dayKey] || []).push(f);
    return Object.keys(groups)
      .sort((a, b) => (a < b ? 1 : -1))
      .map((key) => ({ key, label: groups[key][0].dayKey === '00000000' ? '未分类' : groups[key][0].dayLabel, files: groups[key] }));
  },
  download(path) {
    const s = getApp().getSession();
    return new Promise((resolve, reject) => {
      wx.downloadFile({
        url: s.baseUrl.replace(/\/+$/, '') + path,
        header: { Authorization: authHeader() },
        success: resolve,
        fail: (err) => reject(new Error((err && err.errMsg) || 'download fail'))
      });
    });
  },
  // 派生图通用逻辑：优先取服务端缩略图/预览图；老文件没有时下载原图、
  // 压缩一张回传并缓存；视频无封面保持占位
  async fetchDerived(f, dav, derivedPath, makeFn, hasDerived) {
    const ck = cacheKeyForPath(derivedPath);
    const cached = thumbCache.get(ck);
    if (cached) return cached;
    if (hasDerived) {
      // 服务端确认派生图存在：直接下载，不再发探测请求
      try {
        const res = await this.download(derivedPath);
        if (res.statusCode === 200) return thumbCache.put(ck, res.tempFilePath);
      } catch (e) { console.warn('[month] derived download fail', derivedPath, e); }
      // 标记有但下载失败：视频直接放弃，图片继续走原图生成兜底
      if (f.type === 'video') return null;
    } else if (hasDerived === undefined) {
      // 旧版桥没有 hasXxx 字段：保留 depth=0 探测兼容
      try {
        const found = await dav.propfind(derivedPath, 0);
        if (found && found.length) {
          const res = await this.download(derivedPath);
          if (res.statusCode === 200) return thumbCache.put(ck, res.tempFilePath);
        }
      } catch (e) { console.warn('[month] derived miss', derivedPath, e); /* 继续走原图 */ }
    }
    // hasDerived === false：服务端确认没有派生图，直接走原图生成
    if (f.type === 'video') return null;
    const full = await this.download(f.path);
    if (full.statusCode !== 200) return null;
    const derived = await makeFn(full.tempFilePath);
    if (derived) {
      try { await dav.upload(derivedPath, derived); } catch (e) { /* 不阻塞 */ }
      return thumbCache.put(ck, derived);
    }
    return full.tempFilePath;
  },
  fetchThumb(f, dav) {
    return this.fetchDerived(f, dav, thumbPathFor(f.path), makeImageThumb, f.hasThumb);
  },
  fetchPreview(f, dav) {
    return this.fetchDerived(f, dav, previewPathFor(f.path), makeImagePreview, f.hasPreview);
  },
  async loadThumbs(files) {
    const dav = getDav();
    const CONC = 3;
    let i = 0;
    const worker = async () => {
      while (i < files.length) {
        const f = files[i++];
        let thumb = '';
        let err = '';
        try { thumb = (await this.fetchThumb(f, dav)) || ''; } catch (e) { thumb = ''; err = (e && e.message) || '加载失败'; console.warn('[month] thumb fail', f.path, e); }
        this.setData({
          days: this.data.days.map((day) => ({
            ...day,
            files: day.files.map((x) => x.path === f.path ? Object.assign({}, x, { thumb, status: thumb ? 'ok' : 'error', error: err }) : x)
          }))
        });
      }
    };
    await Promise.all(Array.from({ length: Math.min(CONC, files.length) }, () => worker()));
  },
  findFile(path) {
    for (const day of this.data.days) {
      const f = day.files.find((x) => x.path === path);
      if (f) return f;
    }
    return null;
  },
  noop() {},
  closeVideo() {
    this.setData({ videoSrc: '', playingName: '' });
  },
  onToggleEdit() {
    this.setData({ editing: !this.data.editing, selected: {}, selectedCount: 0 });
  },
  onTapFile(e) {
    const path = e.currentTarget.dataset.path;
    if (this.data.editing) {
      const selected = Object.assign({}, this.data.selected);
      if (selected[path]) delete selected[path];
      else selected[path] = true;
      this.setData({ selected, selectedCount: Object.keys(selected).length });
      return;
    }
    const f = this.findFile(path);
    if (!f) return;
    if (f.type === 'image') {
      this.previewDay(f);
      return;
    }
    wx.showLoading({ title: '下载中' });
    const dav = getDav();
    // 优先播服务端已有的 .preview.mp4 压缩版（流量小）；老视频没有时回退原片。
    // 新版桥 ls 直接带 hasPreviewVideo 标记，省掉一次 depth=0 探测；旧版桥
    // 字段为 undefined 时保留探测兼容。
    let target;
    if (f.hasPreviewVideo === true) {
      target = Promise.resolve(f.previewVideoPath);
    } else if (f.hasPreviewVideo === undefined && f.previewVideoPath) {
      target = dav.propfind(f.previewVideoPath, 0)
        .then((r) => (r && r.length ? f.previewVideoPath : f.path))
        .catch(() => f.path);
    } else {
      target = Promise.resolve(f.path);
    }
    target.then((p) => this.download(p)).then((res) => {
      wx.hideLoading();
      if (res.statusCode !== 200) { wx.showToast({ title: '下载失败 ' + res.statusCode, icon: 'none' }); return; }
      this.setData({ videoSrc: res.tempFilePath, playingName: f.name });
    }).catch(() => {
      wx.hideLoading();
      wx.showToast({ title: '下载失败', icon: 'none' });
    });
  },
  // 点图片 → 下载“当天”所有 1280px 预览图（并发 3，无预览图的老图首次
  // 下载原图压缩回传）→ wx.previewImage 可左右滑动
  async previewDay(f) {
    const day = this.data.days.find((d) => d.files.some((x) => x.path === f.path));
    if (!day) return;
    const images = day.files.filter((x) => x.type === 'image');
    const idx = images.indexOf(f);
    const urls = new Array(images.length);
    let done = 0;
    const dav = getDav();
    wx.showLoading({ title: '加载预览 0/' + images.length });
    let i = 0;
    const CONC = 3;
    const worker = async () => {
      while (i < images.length) {
        const cur = i++;
        try {
          const local = await this.fetchPreview(images[cur], dav);
          if (local) urls[cur] = local;
        } catch (e) { console.warn('[month] preview fail', images[cur].path, e); /* 单张失败跳过，其余仍可预览 */ }
        done += 1;
        wx.showLoading({ title: '加载预览 ' + done + '/' + images.length });
      }
    };
    await Promise.all(Array.from({ length: Math.min(CONC, images.length) }, () => worker()));
    wx.hideLoading();
    const ok = urls.filter(Boolean);
    if (!ok.length) { wx.showToast({ title: '下载失败', icon: 'none' }); return; }
    wx.previewImage({ current: urls[idx] || urls[0], urls: ok });
  },
  async onDeleteSelected() {
    const paths = Object.keys(this.data.selected);
    if (!paths.length) return;
    const ok = await new Promise((resolve) =>
      wx.showModal({ title: '删除', content: '确认删除选中的 ' + paths.length + ' 个文件？', success: (r) => resolve(r.confirm) })
    );
    if (!ok) return;
    const dav = getDav();
    try {
      wx.showLoading({ title: '删除中' });
      for (const p of paths) {
        await dav.del(p);
        // 原图删掉时服务端缩略图/预览图一起删，本地缓存同步清
        try { await dav.del(thumbPathFor(p)); } catch (e) { /* 没有缩略图或已删 */ }
        try { await dav.del(previewPathFor(p)); } catch (e) { /* 没有预览图或已删 */ }
        try { await dav.del(previewVideoPathFor(p)); } catch (e) { /* 没有视频压缩版或已删 */ }
        thumbCache.remove(cacheKeyForPath(thumbPathFor(p)));
        thumbCache.remove(cacheKeyForPath(previewPathFor(p)));
      }
      wx.hideLoading();
      wx.showToast({ title: '已删除', icon: 'success' });
      this.setData({ editing: false });
      this.loadFiles();
    } catch (err) {
      wx.hideLoading();
      wx.showToast({ title: err.message, icon: 'none' });
    }
  },
  saveImageToAlbum(filePath) {
    return new Promise((resolve, reject) =>
      wx.saveImageToPhotosAlbum({ filePath, success: resolve, fail: reject })
    );
  },
  saveVideoToAlbum(filePath) {
    return new Promise((resolve, reject) =>
      wx.saveVideoToPhotosAlbum({ filePath, success: resolve, fail: reject })
    );
  },
  async onDownloadSelected() {
    const paths = Object.keys(this.data.selected);
    if (!paths.length) return;
    const files = paths.map((p) => this.findFile(p)).filter(Boolean);
    const totalBytes = files.reduce((s, f) => s + (f.size || 0), 0);
    const ok = await new Promise((resolve) =>
      wx.showModal({
        title: '下载',
        content: '将下载 ' + files.length + ' 个文件（共 ' + formatBytes(totalBytes) + '），保存到手机相册？',
        success: (r) => resolve(r.confirm)
      })
    );
    if (!ok) return;
    try {
      await new Promise((resolve, reject) =>
        wx.authorize({ scope: 'scope.writePhotosAlbum', success: resolve, fail: reject })
      );
    } catch (e) {
      const open = await new Promise((resolve) =>
        wx.showModal({
          title: '需要相册权限',
          content: '保存图片/视频需要相册权限，是否去设置开启？',
          confirmText: '去设置',
          success: (r) => resolve(r.confirm)
        })
      );
      if (open) wx.openSetting();
      return;
    }
    let saved = 0;
    let savedBytes = 0;
    wx.showLoading({ title: '保存中 0/' + files.length });
    for (const f of files) {
      try {
        const res = await this.download(f.path);
        if (res.statusCode === 200) {
          if (f.type === 'image') await this.saveImageToAlbum(res.tempFilePath);
          else await this.saveVideoToAlbum(res.tempFilePath);
          saved += 1;
          savedBytes += f.size || 0;
          wx.showLoading({ title: '保存中 ' + saved + '/' + files.length + ' · ' + formatBytes(savedBytes) });
        }
      } catch (e) { /* 单个失败继续下一个 */ }
    }
    wx.hideLoading();
    wx.showToast({ title: '已保存 ' + saved + ' 个', icon: 'success' });
  }
});
