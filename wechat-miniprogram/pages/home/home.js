const { getDav } = require('../../utils/session');
const { isThumbPath, isPreviewPath } = require('../../utils/uploader');

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
      const dirs = items.filter((it) => it.isDir && it.href !== root);
      // 逐月数文件是 N+1 次 propfind，串行在公网 IPv6 下太慢；并发 4 路拉取
      const CONC = 4;
      const months = [];
      let i = 0;
      const worker = async () => {
        while (i < dirs.length) {
          const it = dirs[i++];
          const name = it.href.split('/').filter(Boolean).pop();
          const files = await dav.propfind(it.href, 1) || [];
          const count = files.filter((f) => !f.isDir && !isThumbPath(f.href) && !isPreviewPath(f.href)).length;
          months.push({ name, count, path: it.href });
        }
      };
      await Promise.all(Array.from({ length: Math.min(CONC, dirs.length) }, () => worker()));
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
