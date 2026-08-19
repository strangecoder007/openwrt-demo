const { getDav, authHeader } = require('../../utils/session');
const { thumbPathFor, isThumbPath, makeImageThumb } = require('../../utils/uploader');

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
    try {
      const dav = getDav();
      const items = await dav.propfind(this.data.dirPath, 1) || [];
      const files = items
        .filter((f) => !f.isDir && !isThumbPath(f.href))
        .map((f) => {
          const name = f.href.split('/').filter(Boolean).pop();
          const info = dayInfo(name, f.lastModified);
          return {
            name,
            size: f.contentLength,
            type: f.contentType.startsWith('video/') ? 'video' : 'image',
            path: f.href,
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
      this.loadThumbs(files);
    } catch (e) {
      wx.showToast({ title: e.message || '加载失败', icon: 'none' });
    }
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
        fail: reject
      });
    });
  },
  // 优先取 .thumb.jpg；图片老文件没有缩略图时下载原图、压缩一张回传；视频无封面保持占位
  async fetchThumb(f, dav) {
    const tp = thumbPathFor(f.path);
    try {
      const found = await dav.propfind(tp, 0);
      if (found && found.length) {
        const res = await this.download(tp);
        if (res.statusCode === 200) return res.tempFilePath;
      }
    } catch (e) { /* 继续走原图 */ }
    if (f.type === 'video') return null;
    const full = await this.download(f.path);
    if (full.statusCode !== 200) return null;
    const thumb = await makeImageThumb(full.tempFilePath);
    if (thumb) {
      try { await dav.upload(tp, thumb); } catch (e) { /* 不阻塞 */ }
      return thumb;
    }
    return full.tempFilePath;
  },
  async loadThumbs(files) {
    const dav = getDav();
    const CONC = 3;
    let i = 0;
    const worker = async () => {
      while (i < files.length) {
        const f = files[i++];
        let thumb = '';
        try { thumb = (await this.fetchThumb(f, dav)) || ''; } catch (e) { thumb = ''; }
        this.setData({
          days: this.data.days.map((day) => ({
            ...day,
            files: day.files.map((x) => x.path === f.path ? Object.assign({}, x, { thumb, status: thumb ? 'ok' : 'error' }) : x)
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
      wx.showLoading({ title: '加载中' });
      this.download(f.path).then((res) => {
        wx.hideLoading();
        if (res.statusCode !== 200) { wx.showToast({ title: '下载失败 ' + res.statusCode, icon: 'none' }); return; }
        wx.previewImage({ urls: [res.tempFilePath] });
      }).catch(() => {
        wx.hideLoading();
        wx.showToast({ title: '下载失败', icon: 'none' });
      });
      return;
    }
    wx.showLoading({ title: '下载中' });
    this.download(f.path).then((res) => {
      wx.hideLoading();
      if (res.statusCode !== 200) { wx.showToast({ title: '下载失败 ' + res.statusCode, icon: 'none' }); return; }
      this.setData({ videoSrc: res.tempFilePath, playingName: f.name });
    }).catch(() => {
      wx.hideLoading();
      wx.showToast({ title: '下载失败', icon: 'none' });
    });
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
      for (const p of paths) await dav.del(p);
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
    const ok = await new Promise((resolve) =>
      wx.showModal({ title: '下载', content: '将选中的 ' + paths.length + ' 个文件保存到手机相册？', success: (r) => resolve(r.confirm) })
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
    wx.showLoading({ title: '保存中 0/' + paths.length });
    for (const p of paths) {
      const f = this.findFile(p);
      if (!f) continue;
      try {
        const res = await this.download(f.path);
        if (res.statusCode === 200) {
          if (f.type === 'image') await this.saveImageToAlbum(res.tempFilePath);
          else await this.saveVideoToAlbum(res.tempFilePath);
          saved += 1;
          wx.showLoading({ title: '保存中 ' + saved + '/' + paths.length });
        }
      } catch (e) { /* 单个失败继续下一个 */ }
    }
    wx.hideLoading();
    wx.showToast({ title: '已保存 ' + saved + ' 个', icon: 'success' });
  }
});
