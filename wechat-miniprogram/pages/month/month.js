const { getDav, authHeader } = require('../../utils/session');

Page({
  data: { dirPath: '', files: [], editing: false, selected: {}, selectedCount: 0, videoSrc: '', playingName: '' },
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
      this.setData({ files, selected: {}, selectedCount: 0 });
      this.loadThumbs(files);
    } catch (e) {
      wx.showToast({ title: e.message || '加载失败', icon: 'none' });
    }
  },
  // 一期没有服务端缩略图，直接并发下载原图临时文件做网格展示（上限 3 并发）
  async loadThumbs(files) {
    const s = getApp().getSession();
    const images = files.filter((f) => f.type === 'image');
    const CONC = 3;
    let i = 0;
    const worker = async () => {
      while (i < images.length) {
        const f = images[i++];
        try {
          const res = await new Promise((resolve, reject) => {
            wx.downloadFile({
              url: s.baseUrl.replace(/\/+$/, '') + f.path,
              header: { Authorization: authHeader() },
              success: resolve,
              fail: reject
            });
          });
          if (res.statusCode === 200) {
            const files = this.data.files.map((x) =>
              x.path === f.path ? Object.assign({}, x, { thumb: res.tempFilePath }) : x
            );
            this.setData({ files });
          }
        } catch (e) { /* 保持占位 */ }
      }
    };
    await Promise.all(Array.from({ length: Math.min(CONC, images.length) }, () => worker()));
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
    const f = this.data.files.find((x) => x.path === path);
    if (!f) return;
    if (f.type === 'image') {
      if (f.thumb) { wx.previewImage({ urls: [f.thumb] }); return; }
      wx.showToast({ title: '图片未就绪', icon: 'none' });
      return;
    }
    wx.showLoading({ title: '下载中' });
    const s = getApp().getSession();
    wx.downloadFile({
      url: s.baseUrl.replace(/\/+$/, '') + f.path,
      header: { Authorization: authHeader() },
      success: (res) => {
        wx.hideLoading();
        if (res.statusCode !== 200) { wx.showToast({ title: '下载失败 ' + res.statusCode, icon: 'none' }); return; }
        this.setData({ videoSrc: res.tempFilePath, playingName: f.name });
      },
      fail: () => { wx.hideLoading(); wx.showToast({ title: '下载失败', icon: 'none' }); }
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
  }
});
