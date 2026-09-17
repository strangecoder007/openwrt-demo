const auth = require('./utils/auth');

App({
  version: 'v20260821.2',
  // detailQueue：月视图 → 文件详情页传递的"同一天图片队列"（含已加载的缩略图），
  // 供详情页左右滑切换；只在这两个页面之间用，返回列表时无需清理
  globalData: { session: null, detailQueue: null },
  onLaunch() {
    this.globalData.session = auth.loadSession();
    this.globalData.isConnected = true;
    if (typeof wx.onNetworkStatusChange === 'function') {
      wx.onNetworkStatusChange((res) => {
        this.globalData.isConnected = res.isConnected;
        this.globalData.networkType = res.networkType;
      });
    }
  },
  setSession(s) {
    this.globalData.session = s;
    auth.saveSession(s);
  },
  getSession() {
    return this.globalData.session;
  },
  clearSession() {
    this.globalData.session = null;
    auth.clearSession();
  }
});
