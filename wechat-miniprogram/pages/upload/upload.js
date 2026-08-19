const { getDav } = require('../../utils/session');
const uploader = require('../../utils/uploader');

Page({
  data: { files: [], uploading: false, done: 0, total: 0, percent: 0, filePercent: 0, currentName: '' },
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
            size: t.size,
            sizeLabel: mb >= 1 ? mb.toFixed(1) + 'MB' : (t.size / 1024).toFixed(0) + 'KB',
            type: t.fileType,
            time: Date.now()
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
    this.setData({ uploading: true, done: 0, total: list.length, percent: 0, filePercent: 0, currentName: '' });
    // 真实进度目标 + 平滑展示值：进度条保证会动，不会卡在 0 或显示 NaN/null
    let target = 0;
    let smooth = 0;
    const timer = setInterval(() => {
      smooth += (target - smooth) * 0.3;
      if (target === 100 && 100 - smooth < 2) smooth = 100;
      this.setData({ percent: Math.round(smooth) });
    }, 150);
    try {
      const prepared = list.map((f) => {
        const ext = (f.tempFilePath.split('.').pop() || '').toLowerCase() || (f.type === 'video' ? 'mp4' : 'jpg');
        return {
          name: uploader.makeFileName(f.type, f.time, ext),
          type: f.type,
          size: f.size,
          time: f.time,
          tempFilePath: f.tempFilePath
        };
      });
      const dav = getDav();
      await uploader.uploadFiles({
        dav,
        files: prepared,
        onProgress: (done, total, percent, name) => {
          const pct = Number(percent);
          const safe = isFinite(pct) ? Math.max(0, Math.min(100, pct)) : 0;
          target = Math.round(((done + safe / 100) / total) * 100);
          this.setData({ done, total, filePercent: Math.round(safe), currentName: name || '' });
        }
      });
      target = 100;
      wx.showToast({ title: '上传完成 ' + prepared.length + ' 个', icon: 'success' });
      setTimeout(() => wx.navigateBack(), 800);
    } catch (e) {
      wx.showToast({ title: (e && e.message) || '上传失败', icon: 'none' });
    } finally {
      clearInterval(timer);
      this.setData({ uploading: false });
    }
  }
});
