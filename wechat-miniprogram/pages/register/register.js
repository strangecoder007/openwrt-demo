const auth = require('../../utils/auth');
const { createDav } = require('../../utils/dav');
const { quickRequest, classifyNetError } = require('../../utils/wxreq');
const { BASE_URL } = require('../../utils/config');

Page({
  data: {
    adminUser: 'backup',
    adminPass: '',
    newUser: '',
    newPass: '',
    newPass2: '',
    loading: false
  },
  onLoad() {
    const s = getApp().getSession();
    if (s) this.setData({ adminUser: s.user });
  },
  onInput(e) {
    this.setData({ [e.currentTarget.dataset.field]: e.detail.value });
  },
  async onSubmit() {
    const { adminUser, adminPass, newUser, newPass, newPass2 } = this.data;
    const baseUrl = BASE_URL;
    if (!adminUser || !adminPass || !newUser || !newPass) {
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
        // 同登录页：快请求（单次尝试 + 10s），网络不通时立刻给反馈
        request: quickRequest
      });
      await dav.register(newUser, newPass);
      getApp().setSession({ baseUrl, user: newUser, pass: newPass });
      wx.showToast({ title: '注册成功，已登录', icon: 'success' });
      setTimeout(() => wx.reLaunch({ url: '/pages/home/home' }), 800);
    } catch (e) {
      const raw = (e && e.message) || '';
      const kind = classifyNetError(raw);
      if (e.code === 401) {
        wx.showToast({ title: '管理员账号或密码错误', icon: 'none' });
      } else if (e.code === 409) {
        wx.showToast({ title: '用户名已存在', icon: 'none' });
      } else if (kind === 'domain') {
        wx.showModal({
          title: '域名未校验通过',
          content: '真机调试请在开发者工具「详情 → 本地设置」勾选“不校验合法域名”，或把该域名加入小程序 request 合法域名。',
          showCancel: false,
          confirmText: '知道了'
        });
      } else if (kind === 'connect') {
        wx.showModal({
          title: '连不上服务器',
          content: '请求超时或不可达。该服务目前只有一个 IPv6 地址（无 IPv4），请确认手机当前网络支持 IPv6，或换成有 IPv6 的 Wi-Fi 重试。',
          showCancel: false,
          confirmText: '知道了'
        });
      } else {
        wx.showToast({ title: raw || '注册失败', icon: 'none' });
      }
    } finally {
      this.setData({ loading: false });
    }
  }
});
