const assert = require('assert');
const { createDav } = require('../utils/dav');

function mockRequest(handler) {
  return function request(opts) {
    return Promise.resolve(handler(opts));
  };
}

async function main() {
  const calls = [];
  const dav = createDav({
    baseUrl: 'https://cy.gcaiyy.xyz',
    authHeader: 'Basic eDp4',
    request: mockRequest((opts) => {
      calls.push(opts);
      if (opts.method === 'PUT') return { statusCode: 201, data: '' };
      if (opts.url.indexOf('op=mkdir') !== -1) return { statusCode: 201, data: { ok: true, items: [] } };
      return { statusCode: 200, data: { ok: true, items: [] } };
    })
  });

  // PROPFIND goes through the CGI bridge as a plain GET.
  await dav.propfind('/dav/backup/android/DCIM/', 1);
  assert.strictEqual(calls[0].method, 'GET');
  assert.ok(calls[0].url.startsWith('https://cy.gcaiyy.xyz/cgi-bin/dav-bridge.cgi?op=ls&'));
  assert.ok(calls[0].url.indexOf('path=' + encodeURIComponent('/dav/backup/android/DCIM/')) !== -1);
  assert.ok(calls[0].url.indexOf('depth=1') !== -1);
  assert.strictEqual(calls[0].header.Authorization, 'Basic eDp4');
  assert.strictEqual(calls[0].header.Depth, undefined);

  // Depth-0 existence check.
  await dav.propfind('/dav/backup/android/DCIM/2026-08', 0);
  assert.ok(calls[1].url.indexOf('depth=0') !== -1);

  // MKCOL goes through the bridge too.
  await dav.mkcol('/dav/backup/android/DCIM/2026-08');
  assert.strictEqual(calls[2].method, 'GET');
  assert.ok(calls[2].url.indexOf('op=mkdir') !== -1);
  assert.ok(calls[2].url.indexOf('path=' + encodeURIComponent('/dav/backup/android/DCIM/2026-08')) !== -1);

  // PUT still hits WebDAV directly.
  await dav.put('/dav/a.jpg', new Uint8Array([1, 2, 3]).buffer, 'image/jpeg');
  assert.strictEqual(calls[3].method, 'PUT');
  assert.strictEqual(calls[3].url, 'https://cy.gcaiyy.xyz/dav/a.jpg');
  assert.strictEqual(calls[3].header['Content-Type'], 'image/jpeg');

  const missing = createDav({ baseUrl: 'https://x', authHeader: 'B', request: mockRequest(() => ({ statusCode: 404, data: { ok: false, error: 'not_found' } })) });
  assert.strictEqual(await missing.propfind('/nope', 0), null);

  const unauth = createDav({ baseUrl: 'https://x', authHeader: 'B', request: mockRequest(() => ({ statusCode: 401, data: { ok: false, error: 'unauthorized' } })) });
  let threw = null;
  try { await unauth.propfind('/dav/', 1); } catch (e) { threw = e; }
  assert.strictEqual(threw.code, 401);

  // Upload goes through wx.uploadFile (multipart POST) → op=upload.
  const uploadCalls = [];
  const dav2 = createDav({
    baseUrl: 'https://cy.gcaiyy.xyz',
    authHeader: 'Basic eDp4',
    request: mockRequest(() => ({ statusCode: 200, data: { ok: true, items: [] } })),
    uploadFile: (opts) => {
      uploadCalls.push(opts);
      if (opts.onProgressUpdate) opts.onProgressUpdate({ progress: 42 });
      return Promise.resolve({ statusCode: 201, data: '' });
    }
  });
  const pcts = [];
  await dav2.upload('/dav/backup/android/DCIM/2026-08/IMG_x.jpg', 'wxfile://tmp.jpg', (p) => pcts.push(p));
  assert.strictEqual(uploadCalls[0].url, 'https://cy.gcaiyy.xyz/cgi-bin/dav-bridge.cgi?op=upload&path=' + encodeURIComponent('/dav/backup/android/DCIM/2026-08/IMG_x.jpg'));
  assert.strictEqual(uploadCalls[0].name, 'file');
  assert.strictEqual(uploadCalls[0].filePath, 'wxfile://tmp.jpg');
  assert.strictEqual(uploadCalls[0].header.Authorization, 'Basic eDp4');
  assert.ok(uploadCalls[0].onProgressUpdate);
  assert.deepStrictEqual(pcts, [42]);

  const failUpload = createDav({
    baseUrl: 'https://x',
    authHeader: 'B',
    request: mockRequest(() => ({ statusCode: 200, data: {} })),
    uploadFile: () => Promise.resolve({ statusCode: 401, data: '' })
  });
  let uploadErr = null;
  try { await failUpload.upload('/dav/a.jpg', 'wxfile://a.jpg'); } catch (e) { uploadErr = e; }
  assert.strictEqual(uploadErr.code, 401);
  console.log('test-dav OK');
}

main();
