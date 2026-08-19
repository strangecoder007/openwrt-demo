const auth = require('./utils/auth');

App({
  globalData: { session: null },
  onLaunch() {
    this.globalData.session = auth.loadSession();
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
