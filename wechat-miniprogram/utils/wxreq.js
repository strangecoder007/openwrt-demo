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

// retryOpts 可覆盖默认重试策略：登录/注册这类"必须给用户即时反馈"的请求传
// { retries: 1 } + 短超时，避免 60s×3 的重试把按钮卡在"连接中"近 3 分钟。
function wxRequest(opts, retryOpts) {
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
  }), retryOpts);
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

// 登录/注册专用：单次尝试 + 超时封顶 10s → 最快 10 秒内把结果告诉用户。
// 注意 dav.callUrl 会显式传 timeout: 60000，所以这里是"封顶"而不是"兜底默认"。
// 页面列表/上传仍走默认策略（慢链路靠重试兜底，值得多等）。
const QUICK_TIMEOUT = 10000;
function quickRequest(opts) {
  const t = opts.timeout || QUICK_TIMEOUT;
  return wxRequest(Object.assign({}, opts, { timeout: Math.min(t, QUICK_TIMEOUT) }), { retries: 1 });
}

// 把底层 errMsg 归类，便于给用户可执行的提示
//  - 'domain'：真机常见，域名没进小程序「request 合法域名」
//  - 'connect'：连不上（无路由 / 被丢弃 / 拒绝 / 超时）
function classifyNetError(msg) {
  const m = String(msg || '');
  if (/not in domain list|合法域名|域名校验/i.test(m)) return 'domain';
  if (/unreachable|不可达|refused|拒绝|connect|network|网络|timeout|超时|fail/i.test(m)) return 'connect';
  return 'other';
}

module.exports = { wxRequest, wxUploadFile, quickRequest, classifyNetError };
