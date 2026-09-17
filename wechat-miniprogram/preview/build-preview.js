/* ============================================================
 * 预览构建脚本：把真实 wxss 转成浏览器可渲染的 HTML 对照页
 * 用法: node build-preview.js
 * 产物: ../index.html + ../screens/*.html
 * 规则(来自踩坑记录):
 *  - CSS 只从真实 wxss 读取(NEW=工作区, OLD=git 冻结基线)
 *  - rpx → px: 750rpx = 375px, 乘 0.5
 *  - page{} 选择器改写为 html,body{}
 *  - 基准屏宽 375px 不加 transform(fixed 元素不被破坏);
 *    414/768 用 scale 模拟,同时把可视高度除回去
 * ============================================================ */
'use strict';

var fs = require('fs');
var path = require('path');

var ROOT = path.join(__dirname, '..');      // wechat-miniprogram/
var PREVIEW = __dirname;
var OUT = path.join(PREVIEW, 'screens');
if (!fs.existsSync(OUT)) fs.mkdirSync(OUT, { recursive: true });

function read(p) { return fs.readFileSync(path.join(ROOT, p), 'utf8'); }

/* ---------- wxss → css ---------- */
function rpx2px(css) {
  return css.replace(/(-?\d*\.?\d+)rpx/g, function (m, n) { return (parseFloat(n) * 0.5) + 'px'; });
}
function pageSelector(css) {
  // 只换行首形式的 page{，避免误伤注释
  return css.replace(/(^|\n)([ \t]*)page([ \t]*)\{/g, '$1$2html, body$3{');
}
function wxss2css(css) { return pageSelector(rpx2px(css)); }

/* 暗色块：真实小程序靠 @media (prefers-color-scheme: dark)，浏览器预览里
   无法按 iframe 单独切换，于是把同一个媒体查询块克隆成 [data-theme="dark"] 版本。
   令牌值一字不改 —— 预览和真机的暗色完全同源，只是触发方式不同。
   注意选择器要写到 body：light 主题把变量声明在 html, body 两处，
   只写 html 会被 body 上那份盖住。 */
function darkClone(css) {
  var out = [];
  var re = /@media\s*\(prefers-color-scheme:\s*dark\)\s*\{/g;
  var m;
  while ((m = re.exec(css)) !== null) {
    var i = m.index + m[0].length;
    var depth = 1;
    var start = i;
    while (i < css.length && depth > 0) {
      if (css[i] === '{') depth++;
      else if (css[i] === '}') depth--;
      i++;
    }
    var block = css.slice(start, i - 1);       // 去掉最外层大括号
    out.push(block.replace(/(^|\n)([ \t]*)html,\s*body([ \t]*)\{/g,
      '$1$2html[data-theme="dark"], html[data-theme="dark"] body$3{'));
  }
  return out.join('\n');
}

var NEW_APP = wxss2css(read('app.wxss'));
var NEW_CSS = {
  login: wxss2css(read('pages/login/login.wxss')),
  register: wxss2css(read('pages/register/register.wxss')),
  home: wxss2css(read('pages/home/home.wxss')),
  month: wxss2css(read('pages/month/month.wxss')),
  detail: wxss2css(read('pages/detail/detail.wxss')),
  upload: wxss2css(read('pages/upload/upload.wxss'))
};
var ALL_NEW_PAGE = Object.keys(NEW_CSS).map(function (k) { return NEW_CSS[k]; }).join('\n');

var OLD_APP = wxss2css(fs.readFileSync(path.join(PREVIEW, '_before', 'app.wxss'), 'utf8'));
var OLD_CSS = {
  login: wxss2css(fs.readFileSync(path.join(PREVIEW, '_before', 'pages_login_login.wxss'), 'utf8')),
  home: wxss2css(fs.readFileSync(path.join(PREVIEW, '_before', 'pages_home_home.wxss'), 'utf8')),
  month: wxss2css(fs.readFileSync(path.join(PREVIEW, '_before', 'pages_month_month.wxss'), 'utf8')),
  upload: wxss2css(fs.readFileSync(path.join(PREVIEW, '_before', 'pages_upload_upload.wxss'), 'utf8'))
};
var ALL_OLD_PAGE = Object.keys(OLD_CSS).map(function (k) { return OLD_CSS[k]; }).join('\n');

/* ---------- 公共 RESET / MOCK ---------- */
var RESET = [
  '* { box-sizing: border-box; }',
  'html, body { margin: 0; }',
  'view { display: block; }',
  'text { display: inline; }',
  'image { display: inline-block; }',
  'button { font: inherit; border: 0; background: none; margin: 0; padding: 0 14px; color: inherit; line-height: 2.55; }',
  'button::after { border: none; }',
  'input { font: inherit; outline: none; }',
  '::-webkit-scrollbar { width: 0; height: 0; }'
].join('\n');

var MOCK = [
  '/* ---- 预览占位照片(只动背景) ---- */',
  '.mock-photo { background: linear-gradient(150deg, #8FA98F 0%, #5E7D74 48%, #3C5A50 100%) !important; }',
  '.mock-photo.m2 { background: linear-gradient(160deg, #C9B08A 0%, #96805C 60%, #6B5A3E 100%) !important; }',
  '.mock-photo.m3 { background: linear-gradient(140deg, #7E93B8 0%, #55688C 55%, #374664 100%) !important; }',
  '.mock-photo.m4 { background: linear-gradient(165deg, #B98A7E 0%, #8C5F55 58%, #5F3E37 100%) !important; }',
  '.mock-photo.m5 { background: linear-gradient(135deg, #A3B58C 0%, #71835D 52%, #4A573C 100%) !important; }',
  '.mock-photo.m6 { background: linear-gradient(155deg, #6E8F9E 0%, #46667300 60%, #2E454F 100%) !important; }'
].join('\n');

/* 手机壳：状态栏 + 导航栏(模拟小程序 chrome, 不属于 wxss) */
function chrome(navColor, title, back) {
  var backHtml = back ? '<div class="nv-back">&#8249;</div>' : '';
  return '<div class="chrome" style="background:' + navColor + '">' +
    '<div class="statusbar"><span>9:41</span><span>&#9679;&#9679;&#9679;&#9679; &#128267;</span></div>' +
    '<div class="navbar">' + backHtml + '<div class="nv-title">' + title + '</div><div class="nv-side"></div></div>' +
    '</div>';
}
var CHROME_CSS = [
  '.chrome { position: sticky; top: 0; z-index: 50; }',
  '.statusbar { display: flex; justify-content: space-between; padding: 12px 22px 6px; color: #fff; font-size: 12px; font-weight: 600; }',
  '.navbar { display: flex; align-items: center; height: 44px; padding: 0 12px; color: #fff; }',
  '.nv-back { font-size: 26px; line-height: 1; padding: 0 6px; margin-top: -4px; }',
  '.nv-title { flex: 1; text-align: center; font-size: 16px; font-weight: 600; letter-spacing: 1px; }',
  '.nv-side { width: 30px; }'
].join('\n');

/* ---------- 单屏文档 ---------- */
var THEME_SCRIPT = '<script>(function(){try{if(/theme=dark/.test(location.search))' +
  'document.documentElement.setAttribute("data-theme","dark");}catch(e){}})();</script>';

function doc(opts) {
  var dark = opts.dark ? '\n' + darkClone(opts.css) : '';
  return '<!DOCTYPE html><html><head><meta charset="utf-8">' +
    '<meta name="viewport" content="width=device-width, initial-scale=1">' +
    '<title>' + opts.title + '</title><style>' + RESET + '\n' + CHROME_CSS + '\n' + opts.css + dark + '\n' + MOCK + '</style>' +
    THEME_SCRIPT + '</head>' +
    '<body>' + opts.body + '</body></html>';
}

var svgFolder = "data:image/svg+xml,%3Csvg%20xmlns%3D%27http%3A%2F%2Fwww.w3.org%2F2000%2Fsvg%27%20viewBox%3D%270%200%2024%2024%27%20fill%3D%27none%27%20stroke%3D%27%23D9822B%27%20stroke-width%3D%271.6%27%20stroke-linecap%3D%27round%27%20stroke-linejoin%3D%27round%27%3E%3Cpath%20d%3D%27M3%207a2%202%200%200%201%202-2h4l2%202h8a2%202%200%200%201%202%202v9a2%202%200%200%201-2%202H5a2%202%200%200%201-2-2z%27%2F%3E%3C%2Fsvg%3E";
var svgCamera = "data:image/svg+xml,%3Csvg%20xmlns%3D%27http%3A%2F%2Fwww.w3.org%2F2000%2Fsvg%27%20viewBox%3D%270%200%2024%2024%27%20fill%3D%27none%27%20stroke%3D%27%2317352E%27%20stroke-width%3D%271.6%27%20stroke-linecap%3D%27round%27%20stroke-linejoin%3D%27round%27%3E%3Cpath%20d%3D%27M3%208a2%202%200%200%201%202-2h2l2-2h6l2%202h2a2%202%200%200%201%202%202v10a2%202%200%200%201-2%202H5a2%202%200%200%201-2-2z%27%2F%3E%3Ccircle%20cx%3D%2712%27%20cy%3D%2713%27%20r%3D%274%27%2F%3E%3C%2Fsvg%3E";
var svgFilm = "data:image/svg+xml,%3Csvg%20xmlns%3D%27http%3A%2F%2Fwww.w3.org%2F2000%2Fsvg%27%20viewBox%3D%270%200%2024%2024%27%20fill%3D%27none%27%20stroke%3D%27%2317352E%27%20stroke-width%3D%271.6%27%20stroke-linecap%3D%27round%27%20stroke-linejoin%3D%27round%27%3E%3Crect%20x%3D%272%27%20y%3D%276%27%20width%3D%2714%27%20height%3D%2712%27%20rx%3D%272%27%2F%3E%3Cpath%20d%3D%27M16%2010l6-3v10l-6-3z%27%2F%3E%3C%2Fsvg%3E";
var svgImgIcon = "data:image/svg+xml,%3Csvg%20xmlns%3D%27http%3A%2F%2Fwww.w3.org%2F2000%2Fsvg%27%20viewBox%3D%270%200%2024%2024%27%20fill%3D%27none%27%20stroke%3D%27%233E8E5A%27%20stroke-width%3D%271.6%27%20stroke-linecap%3D%27round%27%20stroke-linejoin%3D%27round%27%3E%3Crect%20x%3D%273%27%20y%3D%274%27%20width%3D%2718%27%20height%3D%2716%27%20rx%3D%272%27%2F%3E%3Ccircle%20cx%3D%279%27%20cy%3D%2710%27%20r%3D%272.2%27%2F%3E%3Cpath%20d%3D%27M3.5%2018l5-5%204%204%203-3%205%205%27%2F%3E%3C%2Fsvg%3E";
var svgVidIcon = "data:image/svg+xml,%3Csvg%20xmlns%3D%27http%3A%2F%2Fwww.w3.org%2F2000%2Fsvg%27%20viewBox%3D%270%200%2024%2024%27%20fill%3D%27none%27%20stroke%3D%27%23B26E1F%27%20stroke-width%3D%271.6%27%20stroke-linecap%3D%27round%27%20stroke-linejoin%3D%27round%27%3E%3Crect%20x%3D%272%27%20y%3D%276%27%20width%3D%2714%27%20height%3D%2712%27%20rx%3D%272%27%2F%3E%3Cpath%20d%3D%27M16%2010l6-3v10l-6-3z%27%2F%3E%3C%2Fsvg%3E";
var svgCloud = "data:image/svg+xml,%3Csvg%20xmlns%3D%27http%3A%2F%2Fwww.w3.org%2F2000%2Fsvg%27%20viewBox%3D%270%200%2024%2024%27%20fill%3D%27none%27%20stroke%3D%27%23E8A656%27%20stroke-width%3D%271.5%27%20stroke-linecap%3D%27round%27%20stroke-linejoin%3D%27round%27%3E%3Cpath%20d%3D%27M17.5%2019a4.5%204.5%200%200%200%200-9h-1.8A7%207%200%201%200%204%2014.7%27%2F%3E%3C%2Fsvg%3E";

var MOODS = ['', 'm2', 'm3', 'm4', 'm5', 'm2', ''];

/* ---------- NEW 屏幕的等价 HTML(按真实 wxml 1:1) ---------- */

function newLogin() {
  return doc({
    title: '登录 · v2',
    dark: true,
    css: NEW_APP + '\n' + NEW_CSS.login,
    body: chrome('#17352E', '云盘', false) +
      '<view class="page login-page">' +
        '<view class="hero"><view class="film-strip"></view>' +
          '<view class="hero-body">' +
            '<view class="logo-ring"><image class="logo-img" src="' + svgCloud + '" mode="aspectFit" /></view>' +
            '<view class="eyebrow">FAMILY ARCHIVE</view>' +
            '<view class="title">我的云盘</view>' +
            '<view class="subtitle">家里的照片，都存在这儿</view>' +
          '</view>' +
        '</view>' +
        '<view class="card form-card">' +
          '<view class="field-group"><text class="field-label">账号</text>' +
            '<input class="field" placeholder="backup" value="backup" /></view>' +
          '<view class="field-group"><text class="field-label">密码</text>' +
            '<input class="field" placeholder="请输入密码" type="password" value="123456" /></view>' +
          '<view class="remember-row"><label class="remember-label"><input type="checkbox" checked style="transform:scale(.85)" /> 记住密码</label></view>' +
          '<button class="btn-primary big">登 录</button>' +
          '<view class="register-link">没有账号？注册一个</view>' +
        '</view>' +
        '<view class="version">云盘 v1.4.0</view>' +
      '</view>'
  });
}

function newHome() {
  var cards = [
    { name: '2026-09', count: 86 },
    { name: '2026-08', count: 132 },
    { name: '2026-07', count: 54 }
  ].map(function (m) {
    return '<view class="month-card">' +
      '<view class="month-icon"><image class="icon-img" src="' + svgFolder + '" mode="aspectFit" /></view>' +
      '<view class="month-info"><view class="month-name">' + m.name + '</view>' +
      '<view class="month-count">' + m.count + ' 个文件</view></view>' +
      '<view class="month-arrow"><view class="chev"></view></view></view>';
  }).join('');
  return doc({
    title: '首页 · v2',
    dark: true,
    css: NEW_APP + '\n' + NEW_CSS.home,
    body: chrome('#17352E', '云盘', false) +
      '<view class="page home-page">' +
        '<view class="hero"><view class="film-strip"></view>' +
          '<view class="hero-body">' +
            '<view class="hero-eyebrow">FAMILY ARCHIVE</view>' +
            '<view class="hero-title">我的云盘</view>' +
            '<view class="hero-sub">3 个月份 · 照片与影像</view>' +
            '<view class="hero-rule"></view>' +
          '</view>' +
        '</view>' +
        '<view class="month-list">' + cards +
          '<view class="month-card skeleton-card">' +
            '<view class="sk-icon skeleton"></view>' +
            '<view class="sk-lines"><view class="sk-line skeleton" style="width:42%"></view>' +
            '<view class="sk-line sk-line-sm skeleton" style="width:30%"></view></view>' +
          '</view>' +
        '</view>' +
        '<view class="fab"><view class="fab-plus">＋</view></view>' +
      '</view>'
  });
}

function newMonth() {
  function cell(mood, video, picked) {
    return '<view class="cell' + (picked ? ' picked' : '') + '">' +
      '<view class="thumb mock-photo ' + mood + '"></view>' +
      (video ? '<view class="video-badge"><view class="tri"></view></view>' : '') +
      '</view>';
  }
  var d1 = ['', 'm2', 'm3', 'm4', 'm5', 'm3'].map(function (m) { return cell(m, false); }).join('');
  var d2 = [cell('m2', true, true), cell('m4', false, false), cell('m5', false, false)];
  return doc({
    title: '文件列表 · v2',
    dark: true,
    css: NEW_APP + '\n' + NEW_CSS.month,
    body: chrome('#17352E', '2026-09', true) +
      '<view class="page month-page">' +
        '<view class="day-section">' +
          '<view class="day-header"><text class="day-label">9月15日</text><text class="day-count">6 个</text><view class="day-rule"></view></view>' +
          '<view class="grid">' + d1 + '</view>' +
        '</view>' +
        '<view class="day-section">' +
          '<view class="day-header"><text class="day-label">9月14日</text><text class="day-count">3 个</text><view class="day-rule"></view></view>' +
          '<view class="grid">' + d2.join('') + '</view>' +
        '</view>' +
        '<view class="day-section">' +
          '<view class="day-header"><text class="day-label">9月12日</text><text class="day-count">2 个</text><view class="day-rule"></view></view>' +
          '<view class="grid">' + cell('m3', false, false) + cell('m4', false, false) + '</view>' +
        '</view>' +
        '<view class="edit-fab">编辑</view>' +
      '</view>'
  });
}

function newDetail(closed) {
  // closed=false：信息面板已展开（看得到上下分区）
  // closed=true ：默认态，媒体铺满整屏 + 「上滑查看信息」把手
  // 真机这一页是 navigationStyle:custom（整页铺满含状态栏，返回按钮自己画），
  // 所以 mock 不套 chrome()。返回按钮/位置指示的 top 用静态值模拟 JS 计算结果。
  var open = !closed;
  var navBtnTop = 14;
  var contentTop = 58;
  return doc({
    title: '文件详情 · v2',
    dark: true,
    css: NEW_APP + '\n' + NEW_CSS.detail + '\n' + PV_DETAIL_CSS,
    body: '<div class="pv-detail"><view class="page detail-page ' + (open ? 'split' : '') + '">' +
        '<view class="media-zone">' +
          '<view class="media-card anim-a"><view class="media-img mock-photo m2"></view></view>' +
          '<view class="nav-back" style="top:' + navBtnTop + 'px">&#8249;</view>' +
          '<view class="pos-pill" style="top:' + contentTop + 'px">3 / 12</view>' +
          (open ? '' :
            '<view class="swipe-hint"><view class="hint-chevron"></view>' +
            '<text class="hint-text">上滑查看信息</text></view>') +
        '</view>' +
        '<view class="info-sheet">' +
          '<view class="grabber"><view class="grabber-bar"></view></view>' +
          '<scroll-view scroll-y class="sheet-body">' +
            '<view class="file-head">' +
              '<view class="file-type pill">照片</view>' +
              '<view class="file-title">IMG_20260915_102340.jpg</view>' +
            '</view>' +
            '<view class="meta-card">' +
              '<view class="meta-row"><text class="meta-label">大小</text><text class="meta-value">3.8 MB</text></view>' +
              '<view class="meta-row"><text class="meta-label">修改时间</text><text class="meta-value">2026-09-15 10:24</text></view>' +
              '<view class="meta-row"><text class="meta-label">存储路径</text><text class="meta-value meta-path">/dav/backup/android/DCIM/2026-09/IMG_20260915_102340.jpg</text></view>' +
            '</view>' +
            '<view class="action-row">' +
              '<button class="btn-primary big save-btn">保存到相册</button>' +
              '<button class="delete-btn">删除文件</button>' +
            '</view>' +
          '</scroll-view>' +
        '</view>' +
      '</view></div>'
  });
}

var PV_DETAIL_CSS = [
  '/* 预览专用覆写：真机这页是 navigationStyle:custom，整页铺满；',
  '   预览 iframe 本身就是整块屏幕，所以 .pv-detail 直接铺满即可。 */',
  '.pv-detail { position: absolute; top: 0; bottom: 0; left: 0; right: 0; }',
  '/* 全局 .page 的 min-height:100vh 会把 .detail-page 撑出 .pv-detail，',
  '   导致媒体区比可视区高出一截、「上滑查看信息」把手被顶出屏幕外 */',
  '.pv-detail .detail-page { position: absolute; top: 0; bottom: 0; min-height: 0; }',
  '/* 预览里的 scroll-view 只是未知元素，不会真的滚动；给它真实滚动行为 */',
  '.pv-detail scroll-view { display: block; overflow-y: auto; }',
  '/* 预览浏览器里 env(safe-area-inset-bottom) 解析失败会让整条 calc 失效，',
  '   操作区会贴到滚动区最底；给个保底间距 */',
  '.pv-detail .action-row { padding-bottom: 14px; }',
  '/* mock 照片默认铺满整屏，看不出媒体区底色；真机照片多为横图，',
  '   aspectFit 居中后上下会露底，这里模拟成 62% 高的横图 */',
  '.pv-detail .media-img { height: 62%; }'
].join('\n');

function newUpload() {
  function row(type, name, size, status, statusHtml, mood) {
    var icon = type === 'video' ? svgVidIcon : svgImgIcon;
    // 有缩略图就铺满图标位（真实 chooseMedia 会返回 thumbTempFilePath）
    var slot = mood
      ? '<view class="fi-thumb mock-photo ' + mood + '"></view>'
      : '<image class="fi-img" src="' + icon + '" mode="aspectFit" />';
    return '<view class="file-row">' +
      '<view class="file-icon ' + type + '">' + slot + '</view>' +
      '<view class="file-info"><view class="file-name">' + name + '</view><view class="file-size">' + size + '</view></view>' +
      '<view class="file-status ' + status + '">' + statusHtml + '</view>' +
      '<view class="file-remove">×</view></view>';
  }
  return doc({
    title: '上传 · v2',
    dark: true,
    css: NEW_APP + '\n' + NEW_CSS.upload,
    body: chrome('#17352E', '上传', true) +
      '<view class="page upload-page">' +
        '<view class="section-title">选择文件</view>' +
        '<view class="btn-row">' +
          '<button class="pick-btn"><image class="pick-icon" src="' + svgCamera + '" mode="aspectFit" /><text class="pick-title">选照片</text><text class="pick-hint">最多 9 张</text></button>' +
          '<button class="pick-btn"><image class="pick-icon" src="' + svgFilm + '" mode="aspectFit" /><text class="pick-title">选视频</text><text class="pick-hint">≤500MB</text></button>' +
        '</view>' +
        '<view class="card">' +
          row('image', '1. 图片', '3.2 MB', 'done', '✓ 完成', 'm3') +
          row('video', '2. 视频', '128.4 MB', 'uploading', '上传中 62%', 'm5') +
          row('image', '3. 图片', '4.1 MB', 'pending', '待上传', 'm2') +
          row('image', '4. 图片', '2.7 MB', 'fail', '失败', 'm4') +
        '</view>' +
        '<view class="card progress-card">' +
          '<view class="progress-head"><text class="progress-label">总进度</text>' +
          '<text class="progress-pct"><text class="pct-num">62</text><text class="pct-unit">%</text></text></view>' +
          '<view class="progress-track"><view class="progress-fill" style="width:62%"></view></view>' +
          '<view class="progress-sub">1/4 · IMG_20260917_091502.jpg（62%）</view>' +
        '</view>' +
        '<button class="btn-primary big" disabled style="opacity:.9">开始上传</button>' +
        '<view class="version">云盘 v1.4.0</view>' +
      '</view>'
  });
}

function newRegister() {
  return doc({
    title: '注册 · v2',
    dark: true,
    css: NEW_APP + '\n' + NEW_CSS.register,
    body: chrome('#17352E', '注册账号', true) +
      '<view class="page register-page">' +
        '<view class="hero"><view class="film-strip"></view>' +
          '<view class="hero-body">' +
            '<view class="logo-ring"><image class="logo-img" src="' + svgCloud + '" mode="aspectFit" /></view>' +
            '<view class="eyebrow">FAMILY ARCHIVE</view>' +
            '<view class="title">注册账号</view>' +
            '<view class="subtitle">新账号与管理员共用同一个云盘</view>' +
          '</view>' +
        '</view>' +
        '<view class="card register-card">' +
          '<view class="field-group"><text class="field-label">管理员账号</text><input class="field" value="backup" /></view>' +
          '<view class="field-group"><text class="field-label">管理员密码（用于开通新账号）</text><input class="field" type="password" placeholder="请输入管理员密码" /></view>' +
          '<view class="divider"><text class="divider-text">以下为新账号信息</text></view>' +
          '<view class="field-group"><text class="field-label">新用户名（3-32 位字母数字）</text><input class="field" placeholder="例如 family" /></view>' +
          '<view class="field-group"><text class="field-label">新密码（至少 6 位）</text><input class="field" type="password" placeholder="请输入密码" /></view>' +
          '<view class="field-group"><text class="field-label">确认新密码</text><input class="field" type="password" placeholder="再输一次" /></view>' +
          '<button class="btn-primary big">注 册</button>' +
          '<view class="tip">注册需要管理员密码；新账号登录后可查看、上传、删除云盘文件。</view>' +
        '</view>' +
      '</view>'
  });
}

/* ---------- OLD 屏幕的等价 HTML ---------- */

function oldLogin() {
  return doc({
    title: '登录 · v1',
    css: OLD_APP + '\n' + OLD_CSS.login,
    body: chrome('#1c3d3a', '云盘', false) +
      '<view class="page login-page">' +
        '<view class="hero"><view class="film-strip"></view>' +
          '<view class="hero-body">' +
            '<view class="logo">☁️</view>' +
            '<view class="title">我的云盘</view>' +
            '<view class="subtitle">家里的照片，都存在这儿</view>' +
          '</view>' +
        '</view>' +
        '<view class="card">' +
          '<view class="field-group"><text class="field-label">服务器地址</text><input class="field" placeholder="https://cy.gcaiyy.xyz:34443" /></view>' +
          '<view class="field-group"><text class="field-label">账号</text><input class="field" value="backup" /></view>' +
          '<view class="field-group"><text class="field-label">密码</text><input class="field" type="password" value="123456" /></view>' +
          '<view class="remember-row"><label class="remember-label"><input type="checkbox" checked style="transform:scale(.85)" /> 记住密码</label></view>' +
          '<button class="btn-primary big">登 录</button>' +
          '<view class="register-link">没有账号？注册一个</view>' +
        '</view>' +
        '<view class="version">云盘 v1.4.0</view>' +
      '</view>'
  });
}

function oldHome() {
  var cards = [
    { name: '2026-09', count: 86 }, { name: '2026-08', count: 132 }, { name: '2026-07', count: 54 }
  ].map(function (m) {
    return '<view class="month-card">' +
      '<view class="month-accent"></view>' +
      '<view class="month-info"><view class="month-name">' + m.name + '</view>' +
      '<view class="month-count">' + m.count + ' 个文件</view></view>' +
      '<view class="month-arrow">›</view></view>';
  }).join('');
  return doc({
    title: '首页 · v1',
    css: OLD_APP + '\n' + OLD_CSS.home,
    body: chrome('#1c3d3a', '云盘', false) +
      '<view class="page home-page">' +
        '<view class="hero"><view class="film-strip"></view>' +
          '<view class="hero-body">' +
            '<view class="hero-title">我的云盘</view>' +
            '<view class="hero-sub">3 个月份备份</view>' +
            '<view class="hero-rule"></view>' +
          '</view>' +
        '</view>' +
        '<view class="month-list">' + cards + '</view>' +
        '<view class="fab"><view class="fab-plus">＋</view></view>' +
      '</view>'
  });
}

function oldMonth() {
  function cell(mood, video) {
    return '<view class="cell">' +
      '<view class="thumb mock-photo ' + mood + '"></view>' +
      (video ? '<view class="video-badge">▶</view>' : '') +
      '</view>';
  }
  var d1 = ['', 'm2', 'm3', 'm4', 'm5', 'm3'].map(function (m) { return cell(m, false); }).join('');
  return doc({
    title: '文件列表 · v1',
    css: OLD_APP + '\n' + OLD_CSS.month,
    body: chrome('#1c3d3a', '2026-09', true) +
      '<view class="page">' +
        '<view class="day-section">' +
          '<view class="day-header"><text class="day-label">9月15日</text><text class="day-count">6 个</text></view>' +
          '<view class="grid">' + d1 + '</view>' +
        '</view>' +
        '<view class="day-section">' +
          '<view class="day-header"><text class="day-label">9月14日</text><text class="day-count">3 个</text></view>' +
          '<view class="grid">' + cell('m2', true) + cell('m4', false) + cell('m5', false) + '</view>' +
        '</view>' +
        '<view class="day-section">' +
          '<view class="day-header"><text class="day-label">9月12日</text><text class="day-count">2 个</text></view>' +
          '<view class="grid">' + cell('m3', false) + cell('m4', false) + '</view>' +
        '</view>' +
        '<view class="edit-fab">编辑</view>' +
      '</view>'
  });
}

function oldUpload() {
  function row(name, size, status, statusHtml) {
    return '<view class="file-row">' +
      '<view class="file-icon ' + (name.indexOf('视频') >= 0 ? 'video' : 'image') + '">' + (name.indexOf('视频') >= 0 ? '🎬' : '🖼') + '</view>' +
      '<view class="file-info"><view class="file-name">' + name + '</view><view class="file-size">' + size + '</view></view>' +
      '<view class="file-status ' + status + '">' + statusHtml + '</view>' +
      '<view class="file-remove">×</view></view>';
  }
  return doc({
    title: '上传 · v1',
    css: OLD_APP + '\n' + OLD_CSS.upload,
    body: chrome('#1c3d3a', '上传', true) +
      '<view class="page upload-page">' +
        '<view class="section-title">选择文件</view>' +
        '<view class="btn-row">' +
          '<button class="btn-outline pick-btn"><text class="pick-emoji">📷</text><text class="pick-title">选照片</text><text class="pick-hint">最多 9 张</text></button>' +
          '<button class="btn-outline pick-btn"><text class="pick-emoji">🎬</text><text class="pick-title">选视频</text><text class="pick-hint">≤500MB</text></button>' +
        '</view>' +
        '<view class="card">' +
          row('1. 图片', '3.2 MB', 'done', '✓ 完成') +
          row('2. 视频', '128.4 MB', 'uploading', '上传中 62%') +
          row('3. 图片', '4.1 MB', 'pending', '待上传') +
        '</view>' +
        '<view class="card progress-card">' +
          '<view class="progress-head"><text class="progress-label">总进度</text><text class="progress-pct">62%</text></view>' +
          '<div class="old-progress"><div style="width:62%"></div></div>' +
          '<view class="progress-sub">1/3 · IMG_20260917_091502.jpg（62%）</view>' +
        '</view>' +
        '<button class="btn-primary big">开始上传</button>' +
        '<view class="version">云盘 v1.4.0</view>' +
      '</view>'
  });
}

/* ---------- 状态一览页 ---------- */
function statesScreen() {
  var css = [
    '.st-sec { margin: 0 0 34px; }',
    '.st-title { font-size: 13px; font-weight: 700; letter-spacing: 2px; color: #5C6F6A; margin: 0 0 16px; }',
    '.st-row { display: flex; flex-wrap: wrap; gap: 12px; align-items: center; margin-bottom: 14px; }',
    '.st-grid3 { display: flex; flex-wrap: wrap; margin: 0 -3.75px; }',
    '.st-grid3 .cell { margin: 3.75px; }',
    '.old-progress { height: 6px; border-radius: 3px; background: #e3eae6; overflow: hidden; }',
    '.old-progress div { height: 100%; background: #d9822b; }'
  ].join('\n');
  function cell(mood, video, picked, checked) {
    return '<view class="cell' + (picked ? ' picked' : '') + '">' +
      '<view class="thumb mock-photo ' + mood + '"></view>' +
      (video ? '<view class="video-badge"><view class="tri"></view></view>' : '') +
      (checked === undefined ? '' : '<view class="check' + (checked ? ' checked' : '') + '">✓</view>') +
      '</view>';
  }
  return doc({
    title: '组件状态一览 · v2',
    dark: true,
    css: NEW_APP + '\n' + ALL_NEW_PAGE + '\n' + css,
    body: chrome('#17352E', '状态一览', false) +
      '<view class="page" style="padding-bottom:80px">' +

      '<view class="st-sec"><view class="st-title">按钮 · 默认 / 按压 / 禁用</view>' +
        '<view class="st-row">' +
          '<button class="btn-primary big" style="width:100%">主按钮（保存到相册）</button>' +
        '</view>' +
        '<view class="st-row">' +
          '<button class="btn-primary">主</button>' +
          '<button class="btn-primary press">主·按压</button>' +
          '<button class="btn-primary" disabled>主·禁用</button>' +
          '<button class="btn-outline">描边</button>' +
          '<button class="btn-outline" disabled>描边·禁用</button>' +
          '<button class="btn-ghost">幽灵</button>' +
          '<button class="btn-danger">危险</button>' +
          '<button class="btn-danger" disabled>危险·禁用</button>' +
        '</view></view>' +

      '<view class="st-sec"><view class="st-title">状态胶囊</view>' +
        '<view class="st-row">' +
          '<text class="pill">待上传</text>' +
          '<text class="file-status uploading" style="display:inline-flex">上传中 62%</text>' +
          '<text class="file-status done" style="display:inline-flex">✓ 完成</text>' +
          '<text class="file-status fail" style="display:inline-flex">失败</text>' +
          '<text class="pill pill-accent">视频</text>' +
          '<text class="pill pill-ok">照片</text>' +
        '</view></view>' +

      '<view class="st-sec"><view class="st-title">网格 · 默认 / 视频角标 / 选中 / 选择态</view>' +
        '<view class="st-grid3">' +
          cell('m2', false) + cell('m3', true) + cell('m4', false, true) +
          cell('m5', false, false, false) + cell('m3', false, false, true) + cell('', false) +
        '</view></view>' +

      '<view class="st-sec"><view class="st-title">骨架屏（加载中）</view>' +
        '<view class="month-card skeleton-card" style="margin-bottom:12px">' +
          '<view class="sk-icon skeleton"></view>' +
          '<view class="sk-lines"><view class="sk-line skeleton" style="width:42%"></view>' +
          '<view class="sk-line sk-line-sm skeleton" style="width:30%"></view></view>' +
        '</view>' +
        '<view class="st-grid3"><view class="cell"><view class="thumb skeleton" style="height:100px"></view></view>' +
        '<view class="cell"><view class="thumb skeleton" style="height:100px"></view></view>' +
        '<view class="cell"><view class="thumb skeleton" style="height:100px"></view></view></view></view>' +

      '<view class="st-sec"><view class="st-title">空态</view>' +
        '<view class="empty" style="margin-top:40px"><view class="empty-icon">📂</view><view class="empty-text">这个月还没有文件</view></view>' +
      '</view>' +

      '<view class="st-sec"><view class="st-title">日期分组头</view>' +
        '<view class="day-header"><text class="day-label">9月15日</text><text class="day-count">6 个</text><view class="day-rule"></view></view>' +
      '</view>' +

      '<view class="st-sec"><view class="st-title">悬浮操作</view>' +
        '<view class="st-row" style="position:relative;height:140px;background:#EEF2EF;border-radius:16px;justify-content:flex-end;padding:0 16px">' +
          '<view class="fab" style="position:relative;right:auto;bottom:auto"><view class="fab-plus">＋</view></view>' +
          '<view class="edit-fab" style="position:relative;right:auto;bottom:auto;margin-left:12px">编辑</view>' +
        '</view></view>' +

      '<view class="st-sec"><view class="st-title">编辑栏（悬浮胶囊）</view>' +
        '<div class="edit-bar" style="position:relative;left:auto;right:auto;bottom:auto">' +
          '<text class="edit-count">已选 <text class="edit-num">3</text> 项</text>' +
          '<button class="btn-outline mini">下载(3)</button>' +
          '<button class="btn-danger">删除(3)</button>' +
          '<button class="btn-ghost">完成</button>' +
        '</div>' +
      '</view>' +

      '</view>'
  });
}

/* ---------- 生成 ---------- */
var SCREENS = [
  { file: 'new-login.html', gen: newLogin },
  { file: 'new-register.html', gen: newRegister },
  { file: 'new-home.html', gen: newHome },
  { file: 'new-month.html', gen: newMonth },
  { file: 'new-detail.html', gen: newDetail },
  { file: 'new-detail-closed.html', gen: function () { return newDetail(true); } },
  { file: 'new-upload.html', gen: newUpload },
  { file: 'old-login.html', gen: oldLogin },
  { file: 'old-home.html', gen: oldHome },
  { file: 'old-month.html', gen: oldMonth },
  { file: 'old-upload.html', gen: oldUpload },
  { file: 'states.html', gen: statesScreen }
];
SCREENS.forEach(function (s) { fs.writeFileSync(path.join(OUT, s.file), s.gen()); });

/* ---------- gallery 外壳 ---------- */
var PHONES_NEW = [
  { file: 'new-home.html', name: '首页', id: 'new-home' },
  { file: 'new-month.html', name: '文件列表', id: 'new-month' },
  { file: 'new-detail.html', name: '文件详情（信息展开）', id: 'new-detail' },
  { file: 'new-detail-closed.html', name: '文件详情（默认态）', id: 'new-detail-closed' },
  { file: 'new-upload.html', name: '上传', id: 'new-upload' },
  { file: 'new-login.html', name: '登录', id: 'new-login' },
  { file: 'new-register.html', name: '注册', id: 'new-register' }
];
var PHONES_OLD = [
  { file: 'old-home.html', name: '首页', id: 'old-home' },
  { file: 'old-month.html', name: '文件列表', id: 'old-month' },
  { file: 'old-upload.html', name: '上传', id: 'old-upload' },
  { file: 'old-login.html', name: '登录', id: 'old-login' }
];
var COMPARE = [
  { pair: 'home', name: '首页', a: 'old-home.html', b: 'new-home.html' },
  { pair: 'month', name: '文件列表', a: 'old-month.html', b: 'new-month.html' },
  { pair: 'upload', name: '上传', a: 'old-upload.html', b: 'new-upload.html' },
  { pair: 'login', name: '登录', a: 'old-login.html', b: 'new-login.html' }
];

function phone(item, extraCls) {
  return '<div class="scaler"><div class="phone ' + (extraCls || '') + '" id="' + item.id + '">' +
    '<iframe src="screens/' + item.file + '" data-src="screens/' + item.file + '" scrolling="yes" frameborder="0"></iframe></div></div>';
}
function compareRow(p) {
  return '<div class="compare-row" id="cmp-' + p.pair + '">' +
    '<div class="cmp-col"><div class="cmp-tag old">当前版本</div>' +
    '<div class="scaler"><div class="phone"><iframe src="screens/' + p.a + '" data-src="screens/' + p.a + '" scrolling="yes" frameborder="0"></iframe></div></div></div>' +
    '<div class="cmp-col"><div class="cmp-tag new">v2 改版</div>' +
    '<div class="scaler"><div class="phone"><iframe src="screens/' + p.b + '" data-src="screens/' + p.b + '" scrolling="yes" frameborder="0"></iframe></div></div></div>' +
    '</div>';
}

var galleryCss = [
  ':root { --shell-bg: #101614; --shell-card: #171f1c; --shell-line: #232e2a; --shell-ink: #e8ede9; --shell-ink2: #8fa39c; --accent: #E09A4B; }',
  '* { box-sizing: border-box; }',
  'body { margin: 0; background: var(--shell-bg); color: var(--shell-ink); font-family: -apple-system, "PingFang SC", "Microsoft YaHei", sans-serif; }',
  '.topbar { position: sticky; top: 0; z-index: 99; background: rgba(16, 22, 20, 0.92); backdrop-filter: blur(10px); border-bottom: 1px solid var(--shell-line); padding: 14px 28px; display: flex; align-items: center; gap: 18px; flex-wrap: wrap; }',
  '.brand { font-size: 15px; font-weight: 700; letter-spacing: 1px; }',
  '.brand em { color: var(--accent); font-style: normal; }',
  '.tabs { display: flex; gap: 8px; margin-left: 12px; }',
  '.tab { padding: 7px 16px; border-radius: 999px; border: 1px solid var(--shell-line); background: transparent; color: var(--shell-ink2); font-size: 13px; cursor: pointer; transition: all .15s ease; }',
  '.tab:hover { color: var(--shell-ink); }',
  '.tab.on { background: var(--accent); border-color: var(--accent); color: #fff; font-weight: 600; }',
  '.sizes { margin-left: auto; display: flex; gap: 6px; align-items: center; }',
  '.sz { padding: 5px 12px; border-radius: 8px; border: 1px solid var(--shell-line); background: transparent; color: var(--shell-ink2); font-size: 12px; cursor: pointer; }',
  '.sz.on { color: var(--accent); border-color: var(--accent); }',
  '.sep { width: 1px; height: 20px; background: var(--shell-line); margin: 0 6px; }',
  '.note { font-size: 12px; color: var(--shell-ink2); margin: 0 0 22px; line-height: 1.7; }',
  '.note code { background: #1d2825; padding: 1px 6px; border-radius: 4px; color: #cfe3dc; }',
  '.wrap { padding: 34px 28px 80px; }',
  '.sec { display: none; }',
  '.sec.on { display: block; }',
  '.sec-title { font-size: 12px; font-weight: 600; letter-spacing: 3px; color: var(--shell-ink2); margin: 0 0 20px; }',
  '.stage { display: flex; flex-wrap: wrap; gap: 34px; align-items: flex-start; }',
  '.col { display: flex; flex-direction: column; }',
  '.col .name { text-align: center; font-size: 13px; color: var(--shell-ink2); margin: 12px 0 0; letter-spacing: 1px; }',
  '.phone { width: 375px; height: 812px; border-radius: 44px; border: 6px solid #26312d; overflow: hidden; background: #fff; box-shadow: 0 24px 60px rgba(0, 0, 0, 0.45); }',
  '.phone iframe { width: 100%; height: 100%; border: 0; }',
  '.scaler { display: inline-block; transition: transform .2s ease; }',
  '.compare-row { display: flex; gap: 34px; margin-bottom: 46px; }',
  '.cmp-col { display: flex; flex-direction: column; }',
  '.cmp-tag { align-self: flex-start; font-size: 12px; font-weight: 600; letter-spacing: 2px; padding: 6px 14px; border-radius: 999px; margin-bottom: 12px; }',
  '.cmp-tag.old { background: #232e2a; color: #8fa39c; }',
  '.cmp-tag.new { background: rgba(224, 154, 75, 0.15); color: var(--accent); border: 1px solid rgba(224, 154, 75, 0.4); }',
  '@media (max-width: 900px) { .compare-row { flex-direction: column; } }'
].join('\n');

var galleryJs = [
  'var SCALES = { 375: 1, 414: 414 / 375, 768: 768 / 375 };',
  'var curScale = 1;',
  'function applyScale(s) {',
  '  curScale = SCALES[s] || 1;',
  '  document.querySelectorAll(".scaler").forEach(function (el) {',
  '    var phone = el.querySelector(".phone");',
  '    if (!phone) return;',
  '    if (curScale === 1) { el.style.transform = ""; el.style.height = ""; phone.style.margin = ""; return; }',
  '    el.style.transformOrigin = "top left";',
  '    el.style.transform = "scale(" + curScale + ")";',
  '    el.style.height = (812 * curScale + 12) + "px";',
  '    el.style.width = (375 * curScale) + "px";',
  '    el.style.overflow = "hidden";',
  '  });',
  '}',
  'function applyTheme(mode) {',
  '  document.querySelectorAll(".phone iframe").forEach(function (f) {',
  '    var base = f.getAttribute("data-src");',
  '    if (!base) return;',
  '    f.src = base + (mode === "dark" ? "?theme=dark" : "");',
  '  });',
  '}',
  'function showTab(id) {',
  '  document.querySelectorAll(".sec").forEach(function (s) { s.classList.toggle("on", s.id === id); });',
  '  document.querySelectorAll(".tab").forEach(function (t) { t.classList.toggle("on", t.dataset.target === id); });',
  '  window.scrollTo({ top: 0 });',
  '}',
  'function deepLink() {',
  '  var h = location.hash.replace("#", "");',
  '  if (!h) return;',
  '  var map = { home: "sec-new", month: "sec-new", detail: "sec-new", upload: "sec-new", login: "sec-new", register: "sec-new" };',
  '  if (h.indexOf("old-") === 0 || map[h]) showTab("sec-new");',
  '  if (h === "states") showTab("sec-states");',
  '  if (h === "compare") showTab("sec-compare");',
  '  var el = document.getElementById(h);',
  '  if (el) setTimeout(function () { el.scrollIntoView({ behavior: "smooth", block: "center" }); }, 60);',
  '}',
  'document.querySelectorAll(".tab").forEach(function (t) {',
  '  t.addEventListener("click", function () { showTab(t.dataset.target); history.replaceState(null, "", "#" + t.dataset.target.replace("sec-", "")); });',
  '});',
  'document.querySelectorAll(".sz").forEach(function (b) {',
  '  b.addEventListener("click", function () {',
  '    document.querySelectorAll(".sz").forEach(function (x) { x.classList.remove("on"); });',
  '    b.classList.add("on");',
  '    applyScale(parseInt(b.dataset.w, 10));',
  '  });',
  '});',
  'document.querySelectorAll(".th").forEach(function (b) {',
  '  b.addEventListener("click", function () {',
  '    document.querySelectorAll(".th").forEach(function (x) { x.classList.remove("on"); });',
  '    b.classList.add("on");',
  '    applyTheme(b.dataset.mode);',
  '  });',
  '});',
  'window.addEventListener("hashchange", deepLink);',
  'deepLink();'
].join('\n');

var gallery = '<!DOCTYPE html><html><head><meta charset="utf-8">' +
  '<title>云盘小程序 · v2 改版预览</title>' +
  '<style>' + galleryCss + '</style></head><body>' +
  '<div class="topbar">' +
    '<div class="brand">云盘小程序 <em>v2</em> · 墨绿档案 Premium</div>' +
    '<div class="tabs">' +
      '<button class="tab on" data-target="sec-new">✦ 新版全览</button>' +
      '<button class="tab" data-target="sec-compare">前后对照</button>' +
      '<button class="tab" data-target="sec-states">组件状态</button>' +
    '</div>' +
    '<div class="sizes">' +
      '<button class="th on" data-mode="light">浅色</button>' +
      '<button class="th" data-mode="dark">深色</button>' +
      '<div class="sep"></div>' +
      '<button class="sz on" data-w="375">375 · iPhone</button>' +
      '<button class="sz" data-w="414">414 · Android</button>' +
      '<button class="sz" data-w="768">768 · 平板</button>' +
    '</div>' +
  '</div>' +
  '<div class="wrap">' +
    '<div class="sec on" id="sec-new">' +
      '<div class="sec-title">NEW DESIGN — 六个页面（登录/注册/首页/文件列表/文件详情/上传）</div>' +
      '<div class="note">深浅色可在右上角切换：小程序真机走 <code>@media (prefers-color-scheme: dark)</code>，' +
      '预览里用同一份令牌块的 <code>[data-theme="dark"]</code> 克隆（值完全相同，只是触发方式不同）。<br>' +
      '旧版对照页没有暗色支持，切深色时保持原样 —— 这也是改版差异之一。</div>' +
      '<div class="stage">' + PHONES_NEW.map(function (p) { return '<div class="col">' + phone(p) + '<div class="name">' + p.name + '</div></div>'; }).join('') + '</div>' +
    '</div>' +
    '<div class="sec" id="sec-compare">' +
      '<div class="sec-title">BEFORE / AFTER — 同屏对照</div>' +
      COMPARE.map(compareRow).join('') +
    '</div>' +
    '<div class="sec" id="sec-states">' +
      '<div class="sec-title">STATES — 按钮八态 / 胶囊 / 网格选中 / 骨架屏 / 空态 / 编辑栏</div>' +
      '<div class="stage"><div class="col">' + phone({ file: 'states.html', id: 'states' }) + '<div class="name">组件状态一览</div></div></div>' +
    '</div>' +
  '</div>' +
  '<script>' + galleryJs + '</script></body></html>';

fs.writeFileSync(path.join(PREVIEW, 'index.html'), gallery);
console.log('done: ' + SCREENS.length + ' screens + index.html');
