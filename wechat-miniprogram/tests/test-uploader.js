const assert = require('assert');
const {
  MAX_VIDEO_BYTES, monthDir, contentTypeFor, thumbPathFor,
  isThumbPath, makeImageThumb, uniquePath, uploadFiles, makeFileName
} = require('../utils/uploader');

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

function testThumbPath() {
  assert.strictEqual(
    thumbPathFor('/dav/backup/android/DCIM/2026-08/a.jpg'),
    '/dav/backup/android/DCIM/2026-08/a.thumb.jpg'
  );
  assert.strictEqual(
    thumbPathFor('/dav/backup/android/DCIM/2026-08/a'),
    '/dav/backup/android/DCIM/2026-08/a.thumb.jpg'
  );
  assert.strictEqual(isThumbPath('/dav/backup/android/DCIM/2026-08/a.thumb.jpg'), true);
  assert.strictEqual(isThumbPath('/dav/backup/android/DCIM/2026-08/a.thumb.thumb.jpg'), true);
  assert.strictEqual(isThumbPath('/dav/backup/android/DCIM/2026-08/a.jpg'), false);
}

async function testNoThumbWithoutWx() {
  // Node 环境没有 wx.compressImage，缩略图生成返回 null，不影响原图上传
  assert.strictEqual(await makeImageThumb('wxfile://a.jpg'), null);
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
  const progress = [];
  const dav = {
    mkcol: async (p) => { log.push('mkcol:' + p); },
    propfind: async () => null,
    upload: async (p, fp, onProgress) => {
      log.push('upload:' + p + '#' + fp);
      if (onProgress) onProgress(50);
    }
  };
  const files = [
    { name: 'a.jpg', type: 'image', size: 100, time: new Date(2026, 7, 18).getTime(), tempFilePath: 'wxfile://a.jpg' },
    { name: 'v.mp4', type: 'video', size: 1024, time: new Date(2026, 7, 19).getTime(), tempFilePath: 'wxfile://v.mp4' }
  ];
  await uploadFiles({ dav, files, onProgress: (done, total, pct) => progress.push(done + '/' + total + '@' + pct) });
  assert.deepStrictEqual(log, [
    'mkcol:/dav/backup/android/DCIM/2026-08',
    'upload:/dav/backup/android/DCIM/2026-08/a.jpg#wxfile://a.jpg',
    'mkcol:/dav/backup/android/DCIM/2026-08',
    'upload:/dav/backup/android/DCIM/2026-08/v.mp4#wxfile://v.mp4'
  ]);
  assert.deepStrictEqual(progress, ['0/2@50', '1/2@100', '1/2@50', '2/2@100']);

  const big = { name: 'b.mp4', type: 'video', size: MAX_VIDEO_BYTES + 1, time: Date.now(), tempFilePath: 'wxfile://b.mp4' };
  let err = null;
  try { await uploadFiles({ dav, files: [big], onProgress: () => {} }); } catch (e) { err = e; }
  assert.strictEqual(err.code, 'TOO_LARGE');
}

testMonthDir(); testContentType(); testMakeFileName(); testThumbPath();
testUniquePath()
  .then(testNoThumbWithoutWx)
  .then(testUploadFiles)
  .then(() => console.log('test-uploader OK'));
