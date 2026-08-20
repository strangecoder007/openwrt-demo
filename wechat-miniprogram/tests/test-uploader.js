const assert = require('assert');
const {
  MAX_VIDEO_BYTES, monthDir, contentTypeFor, thumbPathFor,
  previewPathFor, previewVideoPathFor, isThumbPath, isPreviewPath, makeImageThumb,
  makeImagePreview, makeVideoPreview, uniquePath, uploadFiles, makeFileName
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

function testPreviewPath() {
  assert.strictEqual(
    previewPathFor('/dav/backup/android/DCIM/2026-08/a.jpg'),
    '/dav/backup/android/DCIM/2026-08/a.preview.jpg'
  );
  assert.strictEqual(
    previewPathFor('/dav/backup/android/DCIM/2026-08/a'),
    '/dav/backup/android/DCIM/2026-08/a.preview.jpg'
  );
  assert.strictEqual(isPreviewPath('/dav/backup/android/DCIM/2026-08/a.preview.jpg'), true);
  assert.strictEqual(isPreviewPath('/dav/backup/android/DCIM/2026-08/a.thumb.jpg'), false);
  assert.strictEqual(isPreviewPath('/dav/backup/android/DCIM/2026-08/a.jpg'), false);
  assert.strictEqual(isThumbPath('/dav/backup/android/DCIM/2026-08/a.preview.jpg'), false);
}

function testPreviewVideoPath() {
  assert.strictEqual(
    previewVideoPathFor('/dav/backup/android/DCIM/2026-08/v.mp4'),
    '/dav/backup/android/DCIM/2026-08/v.preview.mp4'
  );
  assert.strictEqual(
    previewVideoPathFor('/dav/backup/android/DCIM/2026-08/v.mov'),
    '/dav/backup/android/DCIM/2026-08/v.preview.mp4'
  );
  assert.strictEqual(
    previewVideoPathFor('/dav/backup/android/DCIM/2026-08/v'),
    '/dav/backup/android/DCIM/2026-08/v.preview.mp4'
  );
  assert.strictEqual(isPreviewPath('/dav/backup/android/DCIM/2026-08/v.preview.mp4'), true);
  assert.strictEqual(isPreviewPath('/dav/backup/android/DCIM/2026-08/a.preview.jpg'), true);
  assert.strictEqual(isPreviewPath('/dav/backup/android/DCIM/2026-08/v.mp4'), false);
  assert.strictEqual(isThumbPath('/dav/backup/android/DCIM/2026-08/v.preview.mp4'), false);
}

async function testNoDerivedWithoutWx() {
  // Node 环境没有 wx.compressImage / wx.compressVideo，派生文件生成返回 null
  assert.strictEqual(await makeImageThumb('wxfile://a.jpg'), null);
  assert.strictEqual(await makeImagePreview('wxfile://a.jpg'), null);
  assert.strictEqual(await makeVideoPreview('wxfile://v.mp4'), null);
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
    { name: 'v.mp4', type: 'video', size: 1024, time: new Date(2026, 7, 19).getTime(), tempFilePath: 'wxfile://v.mp4', thumbTempFilePath: 'wxfile://v_cover.jpg' }
  ];
  await uploadFiles({ dav, files, onProgress: (done, total, pct, name, idx) => progress.push(done + '/' + total + '@' + pct + '#' + idx) });
  assert.deepStrictEqual(log, [
    'mkcol:/dav/backup/android/DCIM/2026-08',
    'upload:/dav/backup/android/DCIM/2026-08/a.jpg#wxfile://a.jpg',
    'mkcol:/dav/backup/android/DCIM/2026-08',
    'upload:/dav/backup/android/DCIM/2026-08/v.mp4#wxfile://v.mp4',
    'upload:/dav/backup/android/DCIM/2026-08/v.thumb.jpg#wxfile://v_cover.jpg'
  ]);
  assert.deepStrictEqual(progress, ['0/2@50#0', '1/2@100#0', '1/2@50#1', '1/2@0#1', '2/2@100#1']);

  const big = { name: 'b.mp4', type: 'video', size: MAX_VIDEO_BYTES + 1, time: Date.now(), tempFilePath: 'wxfile://b.mp4' };
  let err = null;
  try { await uploadFiles({ dav, files: [big], onProgress: () => {} }); } catch (e) { err = e; }
  assert.strictEqual(err.code, 'TOO_LARGE');
}

async function testUploadFilesVideoPreview() {
  // 视频上传后应本地压缩一个 .preview.mp4 回传，再传封面；压缩失败（null）静默跳过
  const log = [];
  const dav = {
    mkcol: async () => {},
    propfind: async () => null,
    upload: async (p, fp) => { log.push('upload:' + p + '#' + fp); }
  };
  const files = [
    { name: 'v.mp4', type: 'video', size: 1024, time: new Date(2026, 7, 19).getTime(), tempFilePath: 'wxfile://v.mp4', thumbTempFilePath: 'wxfile://v_cover.jpg' }
  ];
  await uploadFiles({
    dav, files, onProgress: () => {},
    compressVideo: async (fp) => 'preview-' + fp
  });
  assert.deepStrictEqual(log, [
    'upload:/dav/backup/android/DCIM/2026-08/v.mp4#wxfile://v.mp4',
    'upload:/dav/backup/android/DCIM/2026-08/v.preview.mp4#preview-wxfile://v.mp4',
    'upload:/dav/backup/android/DCIM/2026-08/v.thumb.jpg#wxfile://v_cover.jpg'
  ]);

  // 压缩失败时原片与封面照常上传
  log.length = 0;
  await uploadFiles({ dav, files, onProgress: () => {}, compressVideo: async () => null });
  assert.deepStrictEqual(log, [
    'upload:/dav/backup/android/DCIM/2026-08/v.mp4#wxfile://v.mp4',
    'upload:/dav/backup/android/DCIM/2026-08/v.thumb.jpg#wxfile://v_cover.jpg'
  ]);
}

async function testUploadFilesWithDerived() {
  // 图片上传后应依次回传缩略图与预览图（注入压缩函数，验证路径约定）
  const log = [];
  const dav = {
    mkcol: async () => {},
    propfind: async () => null,
    upload: async (p, fp) => { log.push('upload:' + p + '#' + fp); }
  };
  const files = [
    { name: 'a.jpg', type: 'image', size: 100, time: new Date(2026, 7, 18).getTime(), tempFilePath: 'wxfile://a.jpg' }
  ];
  await uploadFiles({
    dav, files, onProgress: () => {},
    makeThumb: async (fp) => 'thumb-' + fp,
    makePreview: async (fp) => 'preview-' + fp
  });
  assert.deepStrictEqual(log, [
    'upload:/dav/backup/android/DCIM/2026-08/a.jpg#wxfile://a.jpg',
    'upload:/dav/backup/android/DCIM/2026-08/a.thumb.jpg#thumb-wxfile://a.jpg',
    'upload:/dav/backup/android/DCIM/2026-08/a.preview.jpg#preview-wxfile://a.jpg'
  ]);
}

async function testUploadFilesServerRename() {
  // 服务端原子命名可能把同名文件改成 -1：派生图必须挂在返回的最终路径下。
  const log = [];
  const dav = {
    mkcol: async () => {},
    propfind: async () => null,
    upload: async (p, fp) => {
      if (p.indexOf('/a.jpg') !== -1 && fp === 'wxfile://a.jpg') {
        log.push('upload:' + p + '#' + fp);
        return '/dav/backup/android/DCIM/2026-08/a-1.jpg';
      }
      if (p.indexOf('.thumb.jpg') !== -1) { log.push('thumb:' + p); return p; }
      if (p.indexOf('.preview.jpg') !== -1) { log.push('preview:' + p); return p; }
      log.push('upload:' + p + '#' + fp);
      return p;
    }
  };
  const files = [
    { name: 'a.jpg', type: 'image', size: 100, time: new Date(2026, 7, 18).getTime(), tempFilePath: 'wxfile://a.jpg' }
  ];
  await uploadFiles({
    dav, files, onProgress: () => {},
    makeThumb: async (fp) => 'thumb-' + fp,
    makePreview: async (fp) => 'preview-' + fp
  });
  assert.deepStrictEqual(log, [
    'upload:/dav/backup/android/DCIM/2026-08/a.jpg#wxfile://a.jpg',
    'thumb:/dav/backup/android/DCIM/2026-08/a-1.thumb.jpg',
    'preview:/dav/backup/android/DCIM/2026-08/a-1.preview.jpg'
  ]);
}

testMonthDir(); testContentType(); testMakeFileName(); testThumbPath(); testPreviewPath(); testPreviewVideoPath();
testUniquePath()
  .then(testNoDerivedWithoutWx)
  .then(testUploadFiles)
  .then(testUploadFilesVideoPreview)
  .then(testUploadFilesWithDerived)
  .then(testUploadFilesServerRename)
  .then(() => console.log('test-uploader OK'));
