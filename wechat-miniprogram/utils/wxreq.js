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

module.exports = { wxRequest };
