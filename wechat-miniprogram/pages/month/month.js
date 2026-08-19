const { getDav, authHeader } = require('../../utils/session');

Page({
  data: { dirPath: '', files: [], editing: false, videoSrc: '', playingName: '' },
  onLoad(q) {
    this.setData({ dirPath: decodeURIComponent(q.path) });
  },
  onShow() { this.loadFiles(); },
  async loadFiles() {
    try {
      const dav = getDav();
      const items = await dav.propfind(this.data.dirPath, 1) || [];
      const files = items
        .filter((f) => !f.isDir)
        .map((f) => {
          const name = f.href.split('/').filter(Boolean).pop();
          return {
            name,
            size: f.contentLength,
            type: f.contentType.startsWith('video/') ? 'video' : 'image',
            path: f.href
          };
        })
        .sort((a, b) => (a.name < b.name ? 1 : -1));
      this.setData({ files });
    } catch (e) {
      wx.showToast({ title: e.message || '加载失败', icon: 'none' });
    }
  },
  onToggleEdit() { this.setData({ editing: !this.data.editing }); },
  async onTapFile(e) {
    const f = e.currentTarget.dataset.file;
    if (this.data.editing) { this.onDelete(e); return; }
    wx.showLoading({ title: '下载中' });
    try {
      const s = getApp().getSession();
      const res = await new Promise((resolve, reject) => {
        wx.downloadFile({
          url: s.baseUrl.replace(/\/+$/, '') + f.path,
          header: { Authorization: authHeader() },
          success: resolve,
          fail: reject
        });
      });
      wx.hideLoading();
      if (res.statusCode !== 200) throw new Error('下载失败 ' + res.statusCode);
      if (f.type === 'image') {
        wx.previewImage({ urls: [res.tempFilePath] });
      } else {
        this.setData({ videoSrc: res.tempFilePath, playingName: f.name });
      }
    } catch (e) {
      wx.hideLoading();
      wx.showToast({ title: e.message, icon: 'none' });
    }
  },
  async onDelete(e) {
    const f = e.currentTarget.dataset.file;
    const ok = await new Promise((resolve) => wx.showModal({ title: '删除', content: '确认删除 ' + f.name + '？', success: (r) => resolve(r.confirm) }));
    if (!ok) return;
    try {
      await getDav().del(f.path);
      this.loadFiles();
    } catch (err) {
      wx.showToast({ title: err.message, icon: 'none' });
    }
  }
});
