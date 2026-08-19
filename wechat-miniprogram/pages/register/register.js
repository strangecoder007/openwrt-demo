const auth = require('../../utils/auth');
const { createDav } = require('../../utils/dav');
const { wxRequest } = require('../../utils/wxreq');

Page({
  data: {
    baseUrl: '',
    adminUser: 'backup',
    adminPass: '',
    newUser: '',
    newPass: '',
    newPass2: '',
    loading: false
  },
  onLoad() {
    const s = getApp().getSession();
    if (s) this.setData({ baseUrl: s.baseUrl, adminUser: s.user });
  },
  onInput(e) {
    this.setData({ [e.currentTarget.dataset.field]: e.detail.value });
  },
  async onSubmit() {
    const { baseUrl, adminUser, adminPass, newUser, newPass, newPass2 } = this.data;
    if (!baseUrl || !adminUser || !adminPass || !newUser || !newPass) {
      wx.showToast({ title: '请填写完整', icon: 'none' });
      return;
    }
    if (newPass !== newPass2) {
      wx.showToast({ title: '两次密码不一致', icon: 'none' });
      return;
    }
    if (newUser.length < 3 || newUser.length > 32) {
      wx.showToast({ title: '用户名需 3-32 位', icon: 'none' });
      return;
    }
    if (newPass.length < 6) {
      wx.showToast({ title: '密码至少 6 位', icon: 'none' });
      return;
    }
    this.setData({ loading: true });
    try {
      // 管理员 Basic 认证由 lighttpd 在 CGI 前完成，dav-bridge 只负责建账号
      const dav = createDav({
        baseUrl,
        authHeader: auth.makeAuthHeader(adminUser, adminPass),
        request: wxRequest
      });
      await dav.register(newUser, newPass);
      getApp().setSession({ baseUrl, user: newUser, pass: newPass });
      wx.showToast({ title: '注册成功，已登录', icon: 'success' });
      setTimeout(() => wx.reLaunch({ url: '/pages/home/home' }), 800);
    } catch (e) {
      const msg = e.code === 401
        ? '管理员账号或密码错误'
        : (e.code === 409 ? '用户名已存在' : (e.message || '注册失败'));
      wx.showToast({ title: msg, icon: 'none' });
    } finally {
      this.setData({ loading: false });
    }
  }
});
