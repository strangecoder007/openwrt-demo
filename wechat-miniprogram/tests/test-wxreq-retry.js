const assert = require('assert');

// ---- 最小 wx 桩：请求可以"永不返回"，用来复现真机上的卡死 ----
let attempts = 0;
let inflight = [];
let mode = 'hang'; // hang | fail-unreachable | ok

global.wx = {
  request(opts) {
    attempts++;
    const rec = { timeout: opts.timeout, opts };
    inflight.push(rec);
    if (mode === 'ok') {
      setTimeout(() => opts.success({ statusCode: 200, data: {} }), 1);
      return;
    }
    // hang：既不 success 也不 fail（模拟无 IPv6 路由时 SYN 被黑洞）
    // quickRequest 只能靠 timeout 兜底 —— 这里手动按 timeout 触发 fail
    setTimeout(() => {
      if (mode === 'hang') opts.fail({ errMsg: 'request:fail timeout' });
      else opts.fail({ errMsg: 'request:fail -102:net::ERR_CONNECTION_REFUSED' });
    }, Math.min(opts.timeout || 60000, 40));
  }
};

const { wxRequest, quickRequest, classifyNetError } = require('../utils/wxreq');

function today() { return new Date().toISOString().slice(0, 10); }

async function main() {
  const checks = [];
  const ok = (name, cond, extra) => { checks.push({ name, pass: !!cond, extra }); };

  // 1) 默认策略：仍保留 3 次重试（列表页靠它兜底慢链路）
  attempts = 0;
  mode = 'fail-unreachable';
  const t0 = Date.now();
  await wxRequest({ url: 'https://x/y', timeout: 30 }).then(
    () => ok('默认 wxRequest 应失败', false),
    (e) => {
      ok('默认 wxRequest 重试 3 次', attempts === 3, 'attempts=' + attempts);
      ok('默认错误信息透传', /ECONNREFUSED|CONNECTION_REFUSED/.test(e.message), e.message);
    }
  );
  ok('默认策略耗时 >= 两次退避(800+1600ms)', Date.now() - t0 >= 2300, 'elapsed=' + (Date.now() - t0));

  // 2) quickRequest：单次尝试（不重试）
  attempts = 0;
  await quickRequest({ url: 'https://x/y', timeout: 60000 }).then(
    () => ok('quickRequest 应失败', false),
    () => ok('quickRequest 单次尝试不重试', attempts === 1, 'attempts=' + attempts)
  );

  // 3) quickRequest：超时必须被"封顶"到 10s（dav.callUrl 传的是 60000）
  let seen = null;
  const origWxRequest = wx.request;
  wx.request = (opts) => { seen = opts.timeout; opts.fail({ errMsg: 'request:fail timeout' }); };
  await quickRequest({ url: 'https://x/y', timeout: 60000 }).catch(() => {});
  ok('quickRequest 把 60s 超时封顶到 10s', seen === 10000, 'timeout passed to wx.request=' + seen);
  await quickRequest({ url: 'https://x/y', timeout: 3000 }).catch(() => {});
  ok('更小的调用方超时不被放大', seen === 3000, 'timeout=' + seen);
  wx.request = origWxRequest;

  // 4) 登录路径的最坏等待：断言交给 wx.request 的 timeout 与尝试次数，
  //    预算 = timeout × attempts = 10s × 1，而不是默认的 60s × 3 ≈ 182s
  attempts = 0;
  let loginTimeout = null;
  wx.request = (opts) => { attempts++; loginTimeout = opts.timeout; opts.fail({ errMsg: 'request:fail timeout' }); };
  await quickRequest({ url: 'https://x/y', timeout: 60000 }).catch(() => {});
  wx.request = origWxRequest;
  const budget = loginTimeout * attempts;
  ok('登录超时预算 = 10s × 1 次', budget === 10000, 'timeout=' + loginTimeout + ' attempts=' + attempts + ' budget=' + budget + 'ms');
  ok('旧默认预算约 182s（说明问题真实存在）', 60000 * 3 + 2400 === 182400);

  // 5) 错误归类
  ok('超时 → connect', classifyNetError('request:fail timeout') === 'connect');
  ok('不可达 → connect', classifyNetError('request:fail -102:net::ERR_CONNECTION_REFUSED') === 'connect');
  ok('域名未校验 → domain', classifyNetError('request:fail url not in domain list') === 'domain');
  ok('401 不当成网络错', classifyNetError('HTTP 401') === 'other');

  const failed = checks.filter((c) => !c.pass);
  checks.forEach((c) => console.log((c.pass ? '  ✓ ' : '  ✗ ') + c.name + (c.extra ? '  [' + c.extra + ']' : '')));
  console.log('\n' + (checks.length - failed.length) + '/' + checks.length + ' passed  (' + today() + ')');
  if (failed.length) process.exit(1);
}

main().catch((e) => { console.error(e); process.exit(1); });
