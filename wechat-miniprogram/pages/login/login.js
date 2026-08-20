const auth = require('../../utils/auth');
const { createDav } = require('../../utils/dav');
const { wxRequest } = require('../../utils/wxreq');

Page({
  data: {
    baseUrl: 'https://cy.gcaiyy.xyz:34443',
    user: 'backup',
    pass: '',
    loading: false,
    version: ''
  },
  onLoad() {
    const s = getApp().getSession();
    if (s) this.setData({ baseUrl: s.baseUrl, user: s.user });
    this.setData({ version: getApp().version || '' });
  },
  onInput(e) {
    this.setData({ [e.currentTarget.dataset.field]: e.detail.value });
  },
  onGoRegister() {
    wx.navigateTo({ url: '/pages/register/register' });
  },
  async onSubmit() {
    const { baseUrl, user, pass } = this.data;
    if (!baseUrl || !user || !pass) {
      wx.showToast({ title: '请填写完整', icon: 'none' });
      return;
    }
    this.setData({ loading: true });
    try {
      const authHeader = auth.makeAuthHeader(user, pass);
      const dav = createDav({ baseUrl, authHeader, request: wxRequest });
      await dav.propfind('/dav/backup/android/DCIM/', 1);
      getApp().setSession({ baseUrl, user, pass });
      wx.showToast({ title: '登录成功', icon: 'success' });
      wx.reLaunch({ url: '/pages/home/home' });
    } catch (e) {
      let msg;
      if (e.code === 401) {
        msg = '账号或密码错误';
      } else if (/unreachable|不可达/.test(e.message || '')) {
        msg = '网络不可达，请切换网络或稍后重试';
      } else if (/timeout|超时/.test(e.message || '')) {
        msg = '连接超时，请稍后重试';
      } else {
        msg = e.message || '连接失败';
      }
      wx.showToast({ title: msg, icon: 'none' });
    } finally {
      this.setData({ loading: false });
    }
  }
});
