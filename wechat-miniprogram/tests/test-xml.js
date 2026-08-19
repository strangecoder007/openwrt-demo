const assert = require('assert');
const { parsePropfind } = require('../utils/xml');

const SAMPLE = `<?xml version="1.0" encoding="utf-8"?>
<D:multistatus xmlns:D="DAV:" xmlns:ns0="urn:uuid:c2f41010-65b3-11d1-a29f-00aa00c14882/">
<D:response><D:href>/dav/backup/android/DCIM/</D:href><D:propstat><D:prop><D:resourcetype><D:collection/></D:resourcetype></D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>
<D:response><D:href>/dav/backup/android/DCIM/2026-08/</D:href><D:propstat><D:prop><D:resourcetype><D:collection/></D:resourcetype></D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>
<D:response><D:href>/dav/backup/android/DCIM/2026-08/a.jpg</D:href><D:propstat><D:prop><D:getcontentlength>12345</D:getcontentlength><D:getcontenttype>image/jpeg</D:getcontenttype></D:prop><D:status>HTTP/1.1 200 OK</D:status></D:propstat></D:response>
</D:multistatus>`;

const items = parsePropfind(SAMPLE);
assert.strictEqual(items.length, 3);
assert.strictEqual(items[1].href, '/dav/backup/android/DCIM/2026-08/');
assert.strictEqual(items[1].isDir, true);
assert.strictEqual(items[2].isDir, false);
assert.strictEqual(items[2].contentLength, 12345);
assert.strictEqual(items[2].contentType, 'image/jpeg');
console.log('test-xml OK');
