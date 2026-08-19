function wxRequest(opts) {
  return new Promise((resolve, reject) => {
    wx.request({
      url: opts.url,
      method: opts.method,
      data: opts.data,
      header: opts.header,
      timeout: opts.timeout,
      success: (res) => resolve({ statusCode: res.statusCode, data: res.data }),
      fail: (err) => reject(new Error(err.errMsg || 'network error'))
    });
  });
}

function wxUploadFile(opts) {
  return new Promise((resolve, reject) => {
    const task = wx.uploadFile({
      url: opts.url,
      filePath: opts.filePath,
      name: opts.name,
      header: opts.header,
      timeout: opts.timeout,
      success: (res) => resolve({ statusCode: res.statusCode, data: res.data }),
      fail: (err) => reject(new Error(err.errMsg || 'network error'))
    });
    if (opts.onProgressUpdate) task.onProgressUpdate(opts.onProgressUpdate);
  });
}

module.exports = { wxRequest, wxUploadFile };
