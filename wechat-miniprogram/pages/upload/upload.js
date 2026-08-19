const { getDav } = require('../../utils/session');
const uploader = require('../../utils/uploader');

Page({
  data: { files: [], done: 0, uploading: false, log: '' },
  pickImage() { this.pick(['image'], 9); },
  pickVideo() { this.pick(['video'], 1); },
  pick(mediaType, count) {
    wx.chooseMedia({
      count,
      mediaType,
      success: (res) => {
        const files = res.tempFiles.map((t) => ({ path: t.tempFilePath, size: t.size, type: t.fileType, time: Date.now() }));
        this.setData({ files: this.data.files.concat(files), done: 0 });
      }
    });
  },
  async startUpload() {
    const list = this.data.files;
    if (!list.length) { wx.showToast({ title: '先选择文件', icon: 'none' }); return; }
    this.setData({ uploading: true, done: 0, log: '' });
    try {
      const fs = wx.getFileSystemManager();
      const prepared = [];
      for (const f of list) {
        const buf = await new Promise((resolve, reject) => {
          // 不传 encoding：readFile 返回 ArrayBuffer，wx.request PUT 才能原样
          // 发送字节；传 'binary' 会得到字符串，发送时按 UTF-8 编码，>127 的
          // 字节全被膨胀成两个字节，图片/视频必然损坏。
          fs.readFile({ filePath: f.path, success: (r) => resolve(r.data), fail: reject });
        });
        const ext = (f.path.split('.').pop() || '').toLowerCase() || (f.type === 'video' ? 'mp4' : 'jpg');
        const name = uploader.makeFileName(f.type, f.time, ext);
        prepared.push({ name, type: f.type, size: f.size, time: f.time, arrayBuffer: buf });
      }
      const dav = getDav();
      const total = prepared.length;
      await uploader.uploadFiles({
        dav,
        files: prepared,
        onProgress: (i, n, f) => {
          this.setData({ done: i, log: '正在上传 ' + i + '/' + n + '：' + f.name });
        }
      });
      wx.showToast({ title: '上传完成 ' + total + ' 个', icon: 'success' });
      setTimeout(() => wx.navigateBack(), 800);
    } catch (e) {
      wx.showToast({ title: (e && e.message) || '上传失败', icon: 'none' });
    } finally {
      this.setData({ uploading: false });
    }
  }
});
