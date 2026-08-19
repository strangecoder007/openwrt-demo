// 字节数 → 人类可读大小（1024 进制）；非法输入按 0 处理
function formatBytes(n) {
  if (!isFinite(n) || n < 0) return '0 B';
  if (n < 1024) return Math.round(n) + ' B';
  const units = ['KB', 'MB', 'GB', 'TB'];
  let v = n;
  let i = -1;
  do { v /= 1024; i += 1; } while (v >= 1024 && i < units.length - 1);
  const s = v >= 100 ? Math.round(v) : Math.round(v * 10) / 10;
  return s + ' ' + units[i];
}

module.exports = { formatBytes };
