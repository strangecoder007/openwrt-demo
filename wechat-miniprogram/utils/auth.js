const KEY = 'davSession';

function utf8Bytes(str) {
  const enc = unescape(encodeURIComponent(str));
  const bytes = new Uint8Array(enc.length);
  for (let i = 0; i < enc.length; i++) bytes[i] = enc.charCodeAt(i);
  return bytes;
}

function base64Encode(str) {
  const b64 = 'ABCDEFGHIJKLMNOPQRSTUVWXYZabcdefghijklmnopqrstuvwxyz0123456789+/';
  const bytes = utf8Bytes(str);
  let out = '';
  for (let i = 0; i < bytes.length; i += 3) {
    const b0 = bytes[i];
    const b1 = bytes[i + 1] || 0;
    const b2 = bytes[i + 2] || 0;
    out += b64[b0 >> 2];
    out += b64[((b0 & 3) << 4) | (b1 >> 4)];
    out += i + 1 < bytes.length ? b64[((b1 & 15) << 2) | (b2 >> 6)] : '=';
    out += i + 2 < bytes.length ? b64[b2 & 63] : '=';
  }
  return out;
}

function makeAuthHeader(user, pass) {
  return 'Basic ' + base64Encode(user + ':' + pass);
}

function saveSession(s) { if (typeof wx !== 'undefined') wx.setStorageSync(KEY, s); }
function loadSession() { return typeof wx !== 'undefined' ? (wx.getStorageSync(KEY) || null) : null; }
function clearSession() { if (typeof wx !== 'undefined') wx.removeStorageSync(KEY); }

module.exports = { base64Encode, makeAuthHeader, saveSession, loadSession, clearSession };
