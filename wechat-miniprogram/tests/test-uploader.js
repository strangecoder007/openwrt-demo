const assert = require('assert');
const { MAX_VIDEO_BYTES, monthDir, contentTypeFor, uniquePath, uploadFiles, makeFileName } = require('../utils/uploader');

function testMonthDir() {
  assert.strictEqual(monthDir(new Date(2026, 7, 18, 10, 30).getTime()), '2026-08');
  assert.strictEqual(monthDir(new Date(2027, 0, 1).getTime()), '2027-01');
}

function testContentType() {
  assert.strictEqual(contentTypeFor('a.JPG'), 'image/jpeg');
  assert.strictEqual(contentTypeFor('v.mp4'), 'video/mp4');
  assert.strictEqual(contentTypeFor('x.unknownext'), 'application/octet-stream');
}

function testMakeFileName() {
  const t = new Date(2026, 7, 18, 10, 30, 5).getTime();
  assert.strictEqual(makeFileName('image', t), 'IMG_20260818_103005.jpg');
  assert.strictEqual(makeFileName('image', t, 'png'), 'IMG_20260818_103005.png');
  assert.strictEqual(makeFileName('video', t, 'mov'), 'VID_20260818_103005.mov');
  assert.strictEqual(makeFileName('video', t), 'VID_20260818_103005.mp4');
}

function testUniquePath() {
  let n = 0;
  const dav = { propfind: async () => (n++ < 1 ? [{ href: 'x' }] : null) };
  return uniquePath(dav, '/dav/m', 'a.jpg').then((p) => {
    assert.strictEqual(p, '/dav/m/a-1.jpg');
  });
}

async function testUploadFiles() {
  const log = [];
  const dav = {
    mkcol: async (p) => { log.push('mkcol:' + p); },
    propfind: async () => null,
    put: async (p) => { log.push('put:' + p); }
  };
  const progress = [];
  const files = [
    { name: 'a.jpg', type: 'image', size: 100, time: new Date(2026, 7, 18).getTime(), arrayBuffer: new Uint8Array(1).buffer },
    { name: 'v.mp4', type: 'video', size: 1024, time: new Date(2026, 7, 19).getTime(), arrayBuffer: new Uint8Array(1).buffer }
  ];
  await uploadFiles({ dav, files, onProgress: (i, t) => progress.push(i + '/' + t) });
  assert.deepStrictEqual(log, [
    'mkcol:/dav/backup/android/DCIM/2026-08',
    'put:/dav/backup/android/DCIM/2026-08/a.jpg',
    'mkcol:/dav/backup/android/DCIM/2026-08',
    'put:/dav/backup/android/DCIM/2026-08/v.mp4'
  ]);
  assert.deepStrictEqual(progress, ['1/2', '2/2']);

  const big = { name: 'b.mp4', type: 'video', size: MAX_VIDEO_BYTES + 1, time: Date.now(), arrayBuffer: new Uint8Array(1).buffer };
  let err = null;
  try { await uploadFiles({ dav, files: [big], onProgress: () => {} }); } catch (e) { err = e; }
  assert.strictEqual(err.code, 'TOO_LARGE');
}

testMonthDir(); testContentType(); testMakeFileName();
testUniquePath().then(testUploadFiles).then(() => console.log('test-uploader OK'));
