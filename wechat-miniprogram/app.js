const auth = require('./utils/auth');

App({
  version: 'v20260820.2',
  globalData: { session: null },
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
