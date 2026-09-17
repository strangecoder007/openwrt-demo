const { getDav, authHeader } = require('../../utils/session');
const { thumbPathFor, previewPathFor, previewVideoPathFor, makeImagePreview } = require('../../utils/uploader');
const { cacheKeyForPath, createThumbCache, createWxFs } = require('../../utils/thumbcache');
const { formatBytes } = require('../../utils/format');

// 与 month 页共用的本地持久缓存（同一目录，命中后零网络请求）
const thumbCache = createThumbCache(createWxFs(), wx.env.USER_DATA_PATH + '/thumbcache', { maxBytes: 100 * 1024 * 1024, maxFiles: 500 });

// 手势阈值：横向位移够大且明显强于纵向才算切图；上滑超过 OPEN_DY 才展开信息面板。
// 先锁轴（AXIS_LOCK），避免横滑被误判成上滑、或上滑被误判成切图。
const SWIPE_MIN_X = 60;
const SWIPE_RATIO = 1.2;
const AXIS_LOCK = 12;
const PANEL_DY = 50;

function fmtDate(ts) {
  if (!ts) return '—';
  const t = new Date(ts);
  if (isNaN(t.getTime())) return '—';
  const p = (n) => (n < 10 ? '0' + n : '' + n);
  return t.getFullYear() + '-' + p(t.getMonth() + 1) + '-' + p(t.getDate()) + ' ' + p(t.getHours()) + ':' + p(t.getMinutes());
}

Page({
  data: {
    file: null,
    img: '',
    loading: true,
    saving: false,
    playing: false,
    videoSrc: '',
    // 同组图片队列（来自月视图；为空即单张模式）
    total: 0,
    index: 0,
    posLabel: '',
    dayLabel: '',
    animClass: 'anim-a',
    // 信息面板：false = 媒体铺满整屏；true = 图片上半屏 + 信息下半屏
    infoOpen: false,
    // 自定义导航（navigationStyle: custom）：整页铺满含状态栏，
    // 这两个值用来给返回按钮和位置指示定位，避免顶到状态栏/胶囊里
    statusBarHeight: 20,
    navBtnTop: 26,
    contentTop: 60
  },

  onLoad(q) {
    this.measureNav();
    let file;
    try {
      file = JSON.parse(decodeURIComponent(q.info));
    } catch (e) {
      wx.showToast({ title: '参数错误', icon: 'none' });
      setTimeout(() => wx.navigateBack(), 600);
      return;
    }
    // 队列由月视图放入 globalData；校验队列确实包含当前文件，避免拿到过期队列
    const queue = getApp().globalData.detailQueue;
    let list = [file];
    let index = 0;
    let dayLabel = '';
    if (queue && Array.isArray(queue.list) && queue.list.length) {
      const i = queue.list.findIndex((x) => x.path === file.path);
      if (i >= 0) { list = queue.list; index = i; dayLabel = queue.dayLabel || ''; }
    }
    this.list = list;
    this.setData({
      index,
      total: list.length,
      dayLabel,
      posLabel: list.length > 1 ? (index + 1) + ' / ' + list.length : ''
    });
    this.show(list[index]);
  },

  // 自定义导航的定位：整页铺满含状态栏，返回按钮与右上角胶囊垂直居中，
  // 位置指示放在胶囊下缘之下，避免顶进状态栏或胶囊
  measureNav() {
    let win = { statusBarHeight: 20 };
    try {
      win = wx.getWindowInfo ? wx.getWindowInfo() : wx.getSystemInfoSync();
    } catch (e) { /* 用默认值 */ }
    let capsule = null;
    try { capsule = wx.getMenuButtonBoundingClientRect(); } catch (e) { /* 用默认值 */ }
    let navBtnTop = win.statusBarHeight + 6;
    let contentTop = win.statusBarHeight + 52;
    if (capsule && capsule.height) {
      navBtnTop = capsule.top + (capsule.height - 32) / 2;
      contentTop = capsule.bottom + 10;
    }
    this.setData({
      statusBarHeight: win.statusBarHeight || 0,
      navBtnTop: navBtnTop,
      contentTop: contentTop
    });
  },

  onBack() {
    if (getCurrentPages().length > 1) wx.navigateBack({ delta: 1 });
    else wx.reLaunch({ url: '/pages/home/home' });
  },

  // 渲染某个文件：装饰显示字段 → 设置 data → 拉取预览
  show(f) {
    const decorated = Object.assign({}, f, {
      sizeLabel: formatBytes(f.size || 0),
      dateLabel: fmtDate(f.lastModified)
    });
    this.current = decorated;
    this.setData({ file: decorated, img: f.thumb || '', loading: f.type !== 'video' });
    this.loadMedia(decorated);
  },

  // 切换动效：交替类名，让同一个 keyframes 每次都能重播
  bumpAnim() {
    return this.data.animClass === 'anim-a' ? 'anim-b' : 'anim-a';
  },

  download(path) {
    const s = getApp().getSession();
    return new Promise((resolve, reject) => {
      wx.downloadFile({
        url: s.baseUrl.replace(/\/+$/, '') + path,
        header: { Authorization: authHeader() },
        success: resolve,
        fail: (err) => reject(new Error((err && err.errMsg) || 'download fail'))
      });
    });
  },

  // 图片：优先 1280 预览图（缓存/服务端），老图回退下载原图
  async loadMedia(f) {
    if (!f || f.type === 'video') { this.setData({ loading: false }); return; }
    try {
      const img = await this.fetchPreview(f);
      // 切换很快时只接受"当前这一张"的结果，避免旧请求覆盖新图
      if (!this.current || this.current.path !== f.path) return;
      this.setData({ img: img || this.data.img, loading: false });
    } catch (e) {
      console.warn('[detail] preview fail', f.path, e);
      if (this.current && this.current.path === f.path) this.setData({ loading: false });
    }
  },

  async fetchPreview(f) {
    const dav = getDav();
    const derivedPath = previewPathFor(f.path);
    const ck = cacheKeyForPath(derivedPath);
    const cached = thumbCache.get(ck);
    if (cached) return cached;
    if (f.hasPreview) {
      try {
        const res = await this.download(derivedPath);
        if (res.statusCode === 200) return thumbCache.put(ck, res.tempFilePath);
      } catch (e) { console.warn('[detail] preview download fail', e); }
    } else if (f.hasPreview === undefined) {
      try {
        const found = await dav.propfind(derivedPath, 0);
        if (found && found.length) {
          const res = await this.download(derivedPath);
          if (res.statusCode === 200) return thumbCache.put(ck, res.tempFilePath);
        }
      } catch (e) { console.warn('[detail] derived miss', e); }
    }
    const full = await this.download(f.path);
    if (full.statusCode !== 200) return null;
    const derived = await makeImagePreview(full.tempFilePath);
    if (derived) {
      try { await dav.upload(derivedPath, derived); } catch (e) { /* 不阻塞 */ }
      return thumbCache.put(ck, derived);
    }
    return full.tempFilePath;
  },

  // —— 同组图片左右切换 ——
  switchTo(next) {
    const list = this.list || [];
    if (next < 0 || next >= list.length || next === this.data.index) return;
    this.setData({
      index: next,
      posLabel: (next + 1) + ' / ' + list.length,
      animClass: this.bumpAnim()
    });
    this.show(list[next]);
  },
  onPrev() { this.switchTo(this.data.index - 1); },
  onNext() { this.switchTo(this.data.index + 1); },

  // —— 手势：先锁轴，再按轴分发 ——
  //   x 轴：左右滑切上一张/下一张（仅同组有多张时）
  //   y 轴：上滑唤出信息面板；面板已展开时下滑收起
  // 锁轴很关键：不锁的话一个斜向滑动会同时满足两个条件。
  onTouchStart(e) {
    const t = e.touches[0];
    this._sx = t.clientX;
    this._sy = t.clientY;
    this._axis = '';
  },
  onTouchMove(e) {
    if (this._sx === undefined || this._axis) return;
    const t = e.touches[0];
    const dx = Math.abs(t.clientX - this._sx);
    const dy = Math.abs(t.clientY - this._sy);
    if (Math.max(dx, dy) < AXIS_LOCK) return;
    this._axis = dx > dy ? 'x' : 'y';
  },
  onTouchEnd(e) {
    if (this._sx === undefined) return;
    const t = e.changedTouches[0];
    const dx = t.clientX - this._sx;
    const dy = t.clientY - this._sy;
    const axis = this._axis;
    this._sx = undefined;
    this._axis = '';

    if (axis === 'x') {
      if (Math.abs(dx) < SWIPE_MIN_X) return;
      if (Math.abs(dx) < Math.abs(dy) * SWIPE_RATIO) return;
      this.switchTo(dx < 0 ? this.data.index + 1 : this.data.index - 1);
      return;
    }
    if (axis === 'y') {
      if (dy <= -PANEL_DY && !this.data.infoOpen) { this.setInfo(true); return; }
      if (dy >= PANEL_DY && this.data.infoOpen) { this.setInfo(false); return; }
    }
  },

  setInfo(open) { this.setData({ infoOpen: !!open }); },
  toggleInfo() { this.setData({ infoOpen: !this.data.infoOpen }); },

  // 点大图 → 原生全屏预览：把同组已加载的图一起传入，可左右滑看原图
  onTapImage() {
    const cur = this.data.img;
    if (!cur) return;
    const urls = ((this.list || []).map((x) => x.thumb).filter(Boolean));
    if (urls.length > 1 && urls.indexOf(cur) >= 0) wx.previewImage({ current: cur, urls });
    else wx.previewImage({ current: cur, urls: [cur] });
  },

  // 视频详情：优先播服务端压缩版，老视频回退原片
  async onTapVideo() {
    const f = this.data.file;
    if (!f) return;
    wx.showLoading({ title: '下载中' });
    const dav = getDav();
    try {
      let target = f.path;
      if (f.hasPreviewVideo === true) {
        target = f.previewVideoPath;
      } else if (f.hasPreviewVideo === undefined && f.previewVideoPath) {
        const r = await dav.propfind(f.previewVideoPath, 0).catch(() => null);
        if (r && r.length) target = f.previewVideoPath;
      }
      const res = await this.download(target);
      wx.hideLoading();
      if (res.statusCode !== 200) { wx.showToast({ title: '下载失败 ' + res.statusCode, icon: 'none' }); return; }
      this.setData({ playing: true, videoSrc: res.tempFilePath });
    } catch (e) {
      wx.hideLoading();
      wx.showToast({ title: '下载失败', icon: 'none' });
    }
  },

  closeVideo() { this.setData({ playing: false, videoSrc: '' }); },
  noop() {},

  async onSave() {
    const f = this.data.file;
    if (!f || this.data.saving) return;
    try {
      await new Promise((resolve, reject) =>
        wx.authorize({ scope: 'scope.writePhotosAlbum', success: resolve, fail: reject })
      );
    } catch (e) {
      const open = await new Promise((resolve) =>
        wx.showModal({
          title: '需要相册权限',
          content: '保存到相册需要相册权限，是否去设置开启？',
          confirmText: '去设置',
          success: (r) => resolve(r.confirm)
        })
      );
      if (open) wx.openSetting();
      return;
    }
    this.setData({ saving: true });
    try {
      const res = await this.download(f.path);
      if (res.statusCode !== 200) throw new Error('HTTP ' + res.statusCode);
      if (f.type === 'image') {
        await new Promise((resolve, reject) => wx.saveImageToPhotosAlbum({ filePath: res.tempFilePath, success: resolve, fail: reject }));
      } else {
        await new Promise((resolve, reject) => wx.saveVideoToPhotosAlbum({ filePath: res.tempFilePath, success: resolve, fail: reject }));
      }
      wx.showToast({ title: '已保存到相册', icon: 'success' });
    } catch (e) {
      wx.showToast({ title: (e && e.message) || '保存失败', icon: 'none' });
    } finally {
      this.setData({ saving: false });
    }
  },

  async onDelete() {
    const f = this.data.file;
    if (!f) return;
    const ok = await new Promise((resolve) =>
      wx.showModal({ title: '删除', content: '确认删除「' + f.name + '」？', success: (r) => resolve(r.confirm) })
    );
    if (!ok) return;
    try {
      wx.showLoading({ title: '删除中' });
      await getDav().del(f.path);
      // 服务端桥已连派生文件一起删；本地缓存同步清掉
      thumbCache.remove(cacheKeyForPath(previewPathFor(f.path)));
      thumbCache.remove(cacheKeyForPath(thumbPathFor(f.path)));
      wx.hideLoading();
      wx.showToast({ title: '已删除', icon: 'success' });

      const list = this.list || [];
      if (list.length > 1) {
        // 同组还有别的图：原地切到下一张（删掉的从队列里摘除），比被弹回列表更顺
        const rest = list.filter((x) => x.path !== f.path);
        const nextIdx = Math.min(this.data.index, rest.length - 1);
        this.list = rest;
        this.setData({
          total: rest.length,
          index: nextIdx,
          posLabel: (nextIdx + 1) + ' / ' + rest.length,
          animClass: this.bumpAnim()
        });
        this.show(rest[nextIdx]);
      } else {
        getApp().globalData.detailQueue = null;
        setTimeout(() => wx.navigateBack(), 400);
      }
    } catch (e) {
      wx.hideLoading();
      wx.showToast({ title: (e && e.message) || '删除失败', icon: 'none' });
    }
  }
});
