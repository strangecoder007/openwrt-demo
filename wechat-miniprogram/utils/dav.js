function createDav({ baseUrl, authHeader, request }) {
  const base = baseUrl.replace(/\/+$/, '');

  function urlFor(path) {
    return base + (path.charAt(0) === '/' ? path : '/' + path);
  }

  function bridgeUrl(op, params) {
    const qs = Object.keys(params)
      .map((k) => k + '=' + encodeURIComponent(params[k]))
      .join('&');
    return base + '/cgi-bin/dav-bridge.cgi?op=' + op + '&' + qs;
  }

  async function callUrl(method, url, opts = {}) {
    const res = await request({
      url,
      method,
      header: Object.assign({ Authorization: authHeader }, opts.header || {}),
      data: opts.data,
      timeout: opts.timeout || 60000
    });
    return res;
  }

  async function call(method, path, opts = {}) {
    return callUrl(method, urlFor(path), opts);
  }

  function httpError(code) {
    const err = new Error('HTTP ' + code);
    err.code = code;
    return err;
  }

  // wx.request has no PROPFIND on real devices (fails with "network argv
  // error"), so listing goes through the dav-bridge CGI on the board.
  async function propfind(path, depth) {
    const res = await callUrl('GET', bridgeUrl('ls', { path, depth: String(depth) }));
    if (res.statusCode === 200) return res.data.items;
    if (res.statusCode === 404) return null;
    throw httpError(res.statusCode);
  }

  // Same for MKCOL: the bridge mirrors 201 (created) / 405 (already exists).
  async function mkcol(path) {
    const res = await callUrl('GET', bridgeUrl('mkdir', { path }));
    if (res.statusCode === 201 || res.statusCode === 405 || res.statusCode === 301) return true;
    throw httpError(res.statusCode);
  }

  async function put(path, arrayBuffer, contentType) {
    const res = await call('PUT', path, { data: arrayBuffer, header: { 'Content-Type': contentType } });
    if (res.statusCode === 201 || res.statusCode === 204) return res.statusCode;
    throw httpError(res.statusCode);
  }

  async function del(path) {
    const res = await call('DELETE', path);
    if (res.statusCode === 204 || res.statusCode === 404) return true;
    throw httpError(res.statusCode);
  }

  return { urlFor, propfind, mkcol, put, del };
}

module.exports = { createDav };
