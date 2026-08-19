const { getDav, authHeader } = require('../../utils/session');
const { thumbPathFor, isThumbPath, makeImageThumb } = require('../../utils/uploader');

Page({
  data: { dirPath: '', monthName: '', files: [], editing: false, selected: {}, selectedCount: 0, videoSrc: '', playingName: '' },
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
          return {
            name,
            size: f.contentLength,
            type: f.contentType.startsWith('video/') ? 'video' : 'image',
            path: f.href,
            thumb: '',
            status: 'pending'
          };
        })
        .sort((a, b) => (a.name < b.name ? 1 : -1));
      this.setData({ files, selected: {}, selectedCount: 0 });
      this.loadThumbs(files);
    } catch (e) {
      wx.showToast({ title: e.message || '加载失败', icon: 'none' });
    }
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
  // 优先取 .thumb.jpg；老文件没有缩略图时下载原图、本地压缩一张回传，下次秒开
  async fetchThumb(f, dav) {
    const tp = thumbPathFor(f.path);
    try {
      const found = await dav.propfind(tp, 0);
      if (found && found.length) {
        const res = await this.download(tp);
        if (res.statusCode === 200) return res.tempFilePath;
      }
    } catch (e) { /* 继续走原图 */ }
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
    const images = files.filter((f) => f.type === 'image');
    const CONC = 3;
    let i = 0;
    const worker = async () => {
      while (i < images.length) {
        const f = images[i++];
        let thumb = '';
        try { thumb = (await this.fetchThumb(f, dav)) || ''; } catch (e) { thumb = ''; }
        const next = this.data.files.map((x) =>
          x.path === f.path ? Object.assign({}, x, { thumb, status: thumb ? 'ok' : 'error' }) : x
        );
        this.setData({ files: next });
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
  }
});
