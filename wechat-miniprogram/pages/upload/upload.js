const { getDav } = require('../../utils/session');
const uploader = require('../../utils/uploader');

Page({
  data: { files: [], uploading: false, done: 0, total: 0, percent: 0, filePercent: 0, currentName: '', version: '' },
  onLoad() {
    this.setData({ version: getApp().version || '' });
  },
  pickImage() { this.pick(['image'], 9); },
  pickVideo() { this.pick(['video'], 1); },
  pick(mediaType, count) {
    wx.chooseMedia({
      count,
      mediaType,
      success: (res) => {
        const files = res.tempFiles.map((t) => {
          const mb = t.size / 1024 / 1024;
          return {
            tempFilePath: t.tempFilePath,
            thumbTempFilePath: t.thumbTempFilePath || '',
            size: t.size,
            sizeLabel: mb >= 1 ? mb.toFixed(1) + 'MB' : (t.size / 1024).toFixed(0) + 'KB',
            type: t.fileType,
            time: Date.now(),
            status: 'pending',
            percent: 0
          };
        });
        this.setData({ files: this.data.files.concat(files), done: 0, percent: 0, filePercent: 0 });
      }
    });
  },
  removeFile(e) {
    if (this.data.uploading) return;
    const idx = e.currentTarget.dataset.index;
    const files = this.data.files.slice();
    files.splice(idx, 1);
    this.setData({ files });
  },
  async startUpload() {
    const list = this.data.files;
    if (!list.length) { wx.showToast({ title: '先选择文件', icon: 'none' }); return; }
    // 上次已成功（status==='done'）的不再重传，避免重复文件、也省时间
    const pending = list.filter((f) => f.status !== 'done');
    if (!pending.length) { wx.showToast({ title: '没有待上传文件', icon: 'none' }); return; }
    const files = list.map((f) => Object.assign({}, f, { status: f.status === 'done' ? 'done' : 'pending', percent: 0 }));
    this.setData({ files, uploading: true, done: 0, total: pending.length, percent: 0, filePercent: 0, currentName: '' });
    // 真实进度目标 + 平滑展示值：进度条保证会动，不会卡在 0 或显示 NaN/null
    let target = 0;
    let smooth = 0;
    const timer = setInterval(() => {
      const step = target === 100 ? 0.5 : 0.3;
      smooth += (target - smooth) * step;
      if (target - smooth < 1) smooth = target;
      const shown = Math.round(smooth);
      if (shown !== this.data.percent) this.setData({ percent: shown });
    }, 150);
    try {
      const prepared = list.map((f) => {
        const ext = (f.tempFilePath.split('.').pop() || '').toLowerCase() || (f.type === 'video' ? 'mp4' : 'jpg');
        return {
          name: uploader.makeFileName(f.type, f.time, ext),
          type: f.type,
          size: f.size,
          time: f.time,
          tempFilePath: f.tempFilePath,
          thumbTempFilePath: f.thumbTempFilePath || '',
          status: f.status
        };
      });
      const dav = getDav();
      const result = await uploader.uploadFiles({
        dav,
        files: prepared,
        onProgress: (done, total, percent, name, index) => {
          const pct = Number(percent);
          const safe = isFinite(pct) ? Math.max(0, Math.min(100, pct)) : 0;
          const next = Math.round(Math.min(100, ((done + safe / 100) / total) * 100));
          target = Math.max(target, next); // 只涨不跌，避免换文件时进度回退
          const nextFiles = this.data.files.map((x, i) =>
            i === index ? Object.assign({}, x, { status: safe >= 100 ? 'done' : 'uploading', percent: Math.round(safe) }) : x
          );
          this.setData({ done, total, files: nextFiles, filePercent: Math.round(safe), currentName: name || '' });
        }
      });
      target = 100;
      const failed = result.failed || [];
      // 把最终仍失败的文件标成 fail，用户可看到并再次按“开始上传”只重传这些
      const failSet = new Set(failed.map((r) => r.index));
      if (failSet.size) {
        this.setData({ files: this.data.files.map((x, i) => failSet.has(i) ? Object.assign({}, x, { status: 'fail' }) : x) });
      }
      wx.showToast({
        title: failed.length ? ('完成 ' + result.uploaded + ' 个，失败 ' + failed.length + ' 个') : ('上传完成 ' + result.uploaded + ' 个'),
        icon: failed.length ? 'none' : 'success'
      });
      if (!failed.length) setTimeout(() => wx.navigateBack(), 800);
    } catch (e) {
      wx.showToast({ title: (e && e.message) || '上传失败', icon: 'none' });
    } finally {
      clearInterval(timer);
      this.setData({ uploading: false });
    }
  }
});
