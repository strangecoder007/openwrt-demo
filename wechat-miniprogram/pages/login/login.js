const auth = require('../../utils/auth');
const { createDav } = require('../../utils/dav');
const { quickRequest, classifyNetError } = require('../../utils/wxreq');
const { BASE_URL } = require('../../utils/config');

Page({
  data: {
    // 服务器地址不再出现在表单里：从 utils/config.js 取固定值。
    // 账号预填 'backup' 只是占位提示，用户仍需输入自己的账号。
    user: 'backup',
    pass: '',
    remember: true,
    loading: false,
    version: ''
  },
  onLoad() {
    const s = getApp().getSession();
    if (s) this.setData({ user: s.user });
    // 记住密码：登录页打开时回填上次保存的账号/密码。
    // 旧版本存过 baseUrl，这里故意忽略 —— 地址已固定，避免带回一个废弃域名。
    const saved = auth.loadCredential();
    if (saved) {
      this.setData({
        user: saved.user || this.data.user,
        pass: saved.pass || '',
        remember: true
      });
    }
    this.setData({ version: getApp().version || '' });
  },
  onInput(e) {
    this.setData({ [e.currentTarget.dataset.field]: e.detail.value });
  },
  onToggleRemember(e) {
    this.setData({ remember: !!e.detail.value });
  },
  onGoRegister() {
    wx.navigateTo({ url: '/pages/register/register' });
  },
  async onSubmit() {
    const { user, pass } = this.data;
    const baseUrl = BASE_URL;
    if (!user || !pass) {
      wx.showToast({ title: '请填写账号和密码', icon: 'none' });
      return;
    }
    this.setData({ loading: true });
    try {
      const authHeader = auth.makeAuthHeader(user, pass);
      // 登录用快请求：单次尝试 + 10s 超时。默认策略是 60s×3 重试，
      // 网络不通时按钮会卡在“连接中…”近 3 分钟，登录不该这样。
      const dav = createDav({ baseUrl, authHeader, request: quickRequest });
      await dav.propfind('/dav/backup/android/DCIM/', 1);
      getApp().setSession({ baseUrl, user, pass });
      if (this.data.remember) auth.saveCredential({ baseUrl, user, pass });
      else auth.clearCredential();
      wx.showToast({ title: '登录成功', icon: 'success' });
      wx.reLaunch({ url: '/pages/home/home' });
    } catch (e) {
      const raw = (e && e.message) || '';
      const kind = classifyNetError(raw);
      if (e.code === 401) {
        wx.showToast({ title: '账号或密码错误', icon: 'none' });
      } else if (kind === 'domain') {
        // 真机调试常见：域名没进「request 合法域名」，开发工具里却能通
        wx.showModal({
          title: '域名未校验通过',
          content: '真机调试请在开发者工具「详情 → 本地设置」勾选“不校验合法域名”，或在微信公众平台把 ' + baseUrl.replace(/^https?:\/\//, '').split(':')[0] + ' 加入 request 合法域名。',
          showCancel: false,
          confirmText: '知道了'
        });
      } else if (kind === 'connect') {
        wx.showModal({
          title: '连不上服务器',
          content: '请求超时或不可达。该服务目前只有一个 IPv6 地址（无 IPv4），请确认手机当前网络支持 IPv6（可在手机浏览器打开 http://6.ipw.cn 验证），或换成有 IPv6 的 Wi-Fi 重试。',
          showCancel: false,
          confirmText: '知道了'
        });
      } else {
        wx.showToast({ title: raw || '连接失败', icon: 'none' });
      }
    } finally {
      this.setData({ loading: false });
    }
  }
});
