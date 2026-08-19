function parsePropfind(xml) {
  const out = [];
  const resp = /<(?:\w+:)?response>([\s\S]*?)<\/(?:\w+:)?response>/g;
  let m;
  while ((m = resp.exec(xml))) {
    const block = m[1];
    const href = block.match(/<(?:\w+:)?href>([\s\S]*?)<\/(?:\w+:)?href>/);
    if (!href) continue;
    const len = block.match(/<(?:\w+:)?getcontentlength>(\d+)<\/(?:\w+:)?getcontentlength>/);
    const type = block.match(/<(?:\w+:)?getcontenttype>([\s\S]*?)<\/(?:\w+:)?getcontenttype>/);
    const lm = block.match(/<(?:\w+:)?getlastmodified[^>]*>([\s\S]*?)<\/(?:\w+:)?getlastmodified>/);
    const isDir = /<(?:\w+:)?resourcetype>[\s\S]*?<(?:\w+:)?collection\s*\/>/.test(block);
    out.push({
      href: href[1].trim(),
      contentLength: len ? parseInt(len[1], 10) : null,
      contentType: type ? type[1].trim() : '',
      lastModified: lm ? lm[1].trim() : '',
      isDir
    });
  }
  return out;
}

module.exports = { parsePropfind };
