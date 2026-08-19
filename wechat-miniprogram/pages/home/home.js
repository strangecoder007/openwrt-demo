const { getDav } = require('../../utils/session');

Page({
  data: { months: [], loading: false },
  onShow() {
    const s = getApp().getSession();
    if (!s) { wx.reLaunch({ url: '/pages/login/login' }); return; }
    this.loadMonths();
  },
  async loadMonths() {
    this.setData({ loading: true });
    try {
      const dav = getDav();
      const root = '/dav/backup/android/DCIM/';
      const items = await dav.propfind(root, 1) || [];
      const months = [];
      for (const it of items) {
        if (!it.isDir || it.href === root) continue;
        const name = it.href.split('/').filter(Boolean).pop();
        const files = await dav.propfind(it.href, 1) || [];
        const count = files.filter((f) => !f.isDir).length;
        months.push({ name, count, path: it.href });
      }
      months.sort((a, b) => (a.name < b.name ? 1 : -1));
      this.setData({ months });
    } catch (e) {
      if (e.code === 401) { wx.reLaunch({ url: '/pages/login/login' }); return; }
      wx.showToast({ title: e.message || '加载失败', icon: 'none' });
    } finally {
      this.setData({ loading: false });
    }
  },
  onPullDownRefresh() {
    this.loadMonths().finally(() => wx.stopPullDownRefresh());
  },
  onTapMonth(e) {
    wx.navigateTo({ url: '/pages/month/month?path=' + encodeURIComponent(e.currentTarget.dataset.path) });
  },
  onUpload() {
    wx.navigateTo({ url: '/pages/upload/upload' });
  }
});
