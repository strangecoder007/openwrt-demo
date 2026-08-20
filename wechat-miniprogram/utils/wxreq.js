// 网络层可重试错误：切网 / 无 IPv6 路由时首次请求会直接 fail（不可达），
// 重试让底层重新走 DNS 与连接，有机会落到可用的地址族（IPv4 A 记录或
// IPv6 AAAA），避免用户一进小程序就撞上“网络不可达”。
const RETRYABLE_RE = /unreachable|不可达|timeout|超时|network|网络|refused|拒绝|connection|connect/i;
// 上传只在“连接都没建立起来”时重试；超时/响应丢失不重试，避免服务端已
// 落盘但客户端没收到响应时重传产生重复文件。
const CONNECT_ONLY_RE = /unreachable|不可达|refused|拒绝|network|网络|connection|connect/i;

function sleep(ms) {
  return new Promise((resolve) => setTimeout(resolve, ms));
}

function isRetryable(msg, re) {
  return re.test(msg || '');
}

async function withRetry(fn, { retries = 3, delayMs = 800, retryRe = RETRYABLE_RE } = {}) {
  let lastErr;
  for (let attempt = 0; attempt < retries; attempt++) {
    try {
      return await fn();
    } catch (e) {
      lastErr = e;
      if (!isRetryable(e.message, retryRe)) throw e;
      if (attempt < retries - 1) await sleep(delayMs * (attempt + 1));
    }
  }
  throw lastErr;
}

function wxRequest(opts) {
  return withRetry(() => new Promise((resolve, reject) => {
    wx.request({
      url: opts.url,
      method: opts.method,
      data: opts.data,
      header: opts.header,
      timeout: opts.timeout,
      success: (res) => resolve({ statusCode: res.statusCode, data: res.data }),
      fail: (err) => reject(new Error(err.errMsg || 'network error'))
    });
  }));
}

function wxUploadFile(opts) {
  return withRetry(() => new Promise((resolve, reject) => {
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
  }), { retries: 2, delayMs: 1000, retryRe: CONNECT_ONLY_RE });
}

module.exports = { wxRequest, wxUploadFile };
