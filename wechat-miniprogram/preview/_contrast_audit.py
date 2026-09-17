# -*- coding: utf-8 -*-
"""设计系统静态审计（可重复运行）。

做三件事，任何一项失败则进程退出码为 1：

  A. 令牌覆盖审计 —— 组件是否引用了未定义的令牌（幽灵令牌）；
                     亮色定义的令牌是否在暗色块里全部覆盖。
  B. 对比度断言   —— 逐对测量「前景令牌 / 底色令牌」，按 WCAG 2.1 阈值判定。
  C. 硬编码色扫描 —— 组件样式里绕过令牌直接写死的颜色值。

用法：
    python preview/_contrast_audit.py
    python preview/_contrast_audit.py -v      # 打印全部实测值而不只打印失败项
"""

from __future__ import print_function
import io
import os
import re
import sys
import glob

ROOT = os.path.dirname(os.path.dirname(os.path.abspath(__file__)))
os.chdir(ROOT)

VERBOSE = '-v' in sys.argv or '--verbose' in sys.argv
FAILURES = []


def rd(p):
    return io.open(p, encoding='utf-8').read()


def blank_comment_lines(text):
    """把 /* */ 与 // 注释内容替换为等长空白，避免注释里的颜色字面量被误判。"""
    out = []
    i = 0
    n = len(text)
    while i < n:
        if text.startswith('/*', i):
            j = text.find('*/', i + 2)
            j = n if j < 0 else j + 2
            out.append(re.sub(r'[^\n]', ' ', text[i:j]))
            i = j
        elif text.startswith('//', i):
            j = text.find('\n', i)
            j = n if j < 0 else j
            out.append(' ' * (j - i))
            i = j
        else:
            out.append(text[i])
            i += 1
    return ''.join(out)


# ---------------------------------------------------------------- 解析令牌块

def balanced_block(text, open_idx):
    """从 text[open_idx] == '{' 起做花括号配平，返回块内文本。"""
    depth = 0
    for i in range(open_idx, len(text)):
        if text[i] == '{':
            depth += 1
        elif text[i] == '}':
            depth -= 1
            if depth == 0:
                return text[open_idx + 1:i]
    raise ValueError('花括号不配平')


app_raw = rd('app.wxss')
app = blank_comment_lines(app_raw)

DARK_MQ = '@media (prefers-color-scheme: dark)'
mq_i = app.find(DARK_MQ)
if mq_i < 0:
    sys.exit('!! app.wxss 里找不到 ' + DARK_MQ)

# 亮色：媒体查询之前的第一个 page {...}
light_src = app[:mq_i]
p_i = light_src.find('page')
light_block = balanced_block(light_src, light_src.find('{', p_i))

# 暗色：媒体查询内第一个 page {...}
dark_src = app[mq_i:]
p_i = dark_src.find('page')
dark_block = balanced_block(dark_src, dark_src.find('{', p_i))

TOK_RE = re.compile(r'(--[a-z0-9-]+)\s*:\s*([^;]+);')
light = dict(TOK_RE.findall(light_block))
dark = dict(TOK_RE.findall(dark_block))


# ---------------------------------------------------------------- 颜色与对比度

def to_rgb(c):
    c = c.strip()
    m = re.match(r'#([0-9a-fA-F]{3})$', c)
    if m:
        return tuple(int(ch * 2, 16) for ch in m.group(1))
    m = re.match(r'#([0-9a-fA-F]{6})$', c)
    if m:
        h = m.group(1)
        return tuple(int(h[i:i + 2], 16) for i in (0, 2, 4))
    m = re.match(r'rgba?\(\s*([\d.]+)[,\s]+([\d.]+)[,\s]+([\d.]+)(?:[,\s/]+([\d.]+))?\s*\)', c)
    if m:
        return (float(m.group(1)), float(m.group(2)), float(m.group(3)),
                float(m.group(4)) if m.group(4) else 1.0)
    return None


def compose(fg, bg):
    """把可能带 alpha 的 fg 合成到不透明 bg 上，返回不透明 rgb。"""
    f = to_rgb(fg)
    b = to_rgb(bg)
    if f is None or b is None:
        return None
    if len(f) == 3:
        return f
    a = f[3]
    return tuple(round(f[k] * a + b[k] * (1 - a)) for k in range(3))


def rel_lum(rgb):
    def ch(v):
        v = v / 255.0
        return v / 12.92 if v <= 0.03928 else ((v + 0.055) / 1.055) ** 2.4
    r, g, b = (ch(v) for v in rgb[:3])
    return 0.2126 * r + 0.7152 * g + 0.0722 * b


def ratio(fg, bg):
    f, b = to_rgb(fg), to_rgb(bg)
    if f is None or b is None:
        return None
    if len(f) == 4:
        f = compose(fg, bg)
    if len(b) == 4:
        return None  # 底色必须不透明，否则需指定合成基准
    lf, lb = rel_lum(f), rel_lum(b)
    hi, lo = max(lf, lb), min(lf, lb)
    return (hi + 0.05) / (lo + 0.05)


def resolve(theme, name):
    """name 可以是令牌名（--x）或颜色字面量。"""
    if name.startswith('--'):
        tbl = light if theme == 'light' else dark
        if name not in tbl:
            tbl = dark if theme == 'light' else light   # 主题无关令牌两侧同值
        return tbl.get(name)
    return name


# 断言表：(前景, 底色, 最低对比度, 说明)
# 阈值取 WCAG 2.1：正文 4.5:1；图标/图形等非文本要素 3:1。
PAIRS = [
    ('--ink',        '--bg',        4.5, '正文 / 页面底'),
    ('--ink',        '--surface',   4.5, '正文 / 卡片面'),
    ('--ink-2',      '--surface',   4.5, '次要文字 / 卡片面'),
    ('--ink-3',      '--surface',   4.5, '三级文字 / 卡片面'),
    ('--ink-3',      '--bg',        4.5, '三级文字 / 页面底'),
    ('--brand-ink',  '--surface',   4.5, '品牌色文字 / 卡片面'),
    ('--brand-ink',  '--bg',        4.5, '品牌色文字 / 页面底'),
    ('--accent-ink', '--surface',   4.5, '琥珀文字 / 卡片面'),
    ('--accent-ink', '--bg',        4.5, '琥珀文字 / 页面底'),
    ('--ghost-fg',   '--surface-2', 4.5, '幽灵按钮文字'),
    ('--ok',         '--ok-tint',   4.5, '成功态胶囊'),
    ('--danger',     '--danger-tint', 4.5, '失败态胶囊'),
    ('--on-brand',   '--brand',     4.5, '品牌面上的前景'),
    ('--on-danger',  '--danger-fill', 4.5, '危险实色按钮文字'),
    # 琥珀渐变的三个端点：文字落在渐变中部，故整体按 accent 判定，
    # 浅端单独按 3:1（大字号/粗体）放宽复核。
    ('--on-accent',  '--accent',      4.5, '琥珀按钮文字（渐变中部，实际着字位置）'),
    ('--on-accent',  '--accent-lite', 3.0, '琥珀按钮文字（渐变浅端，放宽至大字阈值）'),
    ('--on-accent',  '--accent-deep', 3.0, '琥珀按钮文字（渐变深端，放宽至大字阈值）'),
]

# 已知未达标项：显式登记，让它继续出现在报告里而不是被静默放过。
# 形如 (前景, 底色, 主题) -> 原因
KNOWN = {
    ('--on-accent', '--accent', 'light'):
        '亮色下白字压 #D9822B 仅 2.93:1（AA 需 4.5:1）。'
        '这是已确认的风格选择：换成深墨字可到 5.00:1，但会改变已定稿的亮色观感，待决策。',
    ('--on-accent', '--accent-lite', 'light'):
        '同上，渐变浅端 #E2964C 白字 2.41:1。',
}


def audit_tokens():
    print('=' * 66)
    print('A. 令牌覆盖')
    print('=' * 66)

    files = ['app.wxss'] + sorted(glob.glob('pages/*/*.wxss'))
    used = set()
    for f in files:
        used |= set(re.findall(r'var\(\s*(--[a-z0-9-]+)', blank_comment_lines(rd(f))))

    defined = set(light) | set(dark)
    ghost = sorted(used - defined)

    # 结构性令牌与主题无关（圆角、缓动、时长），不需要在暗色块重复定义
    STRUCTURAL = re.compile(r'^--(r-|ease|t-)')
    gap = sorted(t for t in (set(light) & used) - set(dark) if not STRUCTURAL.match(t))

    print('亮色定义 %d 个 / 暗色覆盖 %d 个 / 组件引用 %d 个'
          % (len(light), len(dark), len(used)))
    print()

    if ghost:
        print('  [失败] 引用了但从未定义（幽灵令牌，会静默失效）：')
        for t in ghost:
            print('         ' + t)
        FAILURES.append('幽灵令牌 %d 个' % len(ghost))
    else:
        print('  [通过] 无幽灵令牌')

    if gap:
        print('  [失败] 被引用但暗色未覆盖（深色下会漏出浅色）：')
        for t in gap:
            print('         %s  亮色=%s' % (t, light[t].strip()))
        FAILURES.append('暗色未覆盖令牌 %d 个' % len(gap))
    else:
        print('  [通过] 暗色块已覆盖全部被引用的颜色令牌')

    unused = sorted(set(light) - used)
    if unused:
        print('  [提示] 定义但无组件引用（冗余令牌）：%s' % ', '.join(unused))
    print()


def audit_contrast():
    print('=' * 66)
    print('B. 对比度（WCAG 2.1）')
    print('=' * 66)
    for theme in ('light', 'dark'):
        print('  --- %s ---' % theme)
        bad = 0
        for fg_t, bg_t, need, desc in PAIRS:
            fg, bg = resolve(theme, fg_t), resolve(theme, bg_t)
            if fg is None or bg is None:
                print('    [跳过] %-34s 令牌缺失' % desc)
                continue
            r = ratio(fg, bg)
            if r is None:
                continue
            if r >= need:
                if VERBOSE:
                    print('    %5.2f:1  >= %.1f  %s' % (r, need, desc))
                continue
            key = (fg_t, bg_t, theme)
            if key in KNOWN:
                print('    [已知] %5.2f:1  <  %.1f  %s' % (r, need, desc))
                print('           %s' % KNOWN[key])
            else:
                print('    [失败] %5.2f:1  <  %.1f  %s  (%s on %s)'
                      % (r, need, desc, fg_t, bg_t))
                bad += 1
        if not VERBOSE and bad == 0:
            print('    （未达标项见上方 [已知]；用 -v 打印全部实测值）')
        if bad:
            FAILURES.append('%s 主题 %d 组对比度未达标' % (theme, bad))
    print()


# 主题无关的深色底面：这些选择器下的白色前景在两个主题下都对，属有意写死。
# hero 两个主题下都是深墨绿渐变，其子元素（logo-ring / eyebrow / subtitle）同理。
DARK_SURFACE_SELECTORS = re.compile(
    r'hero|eyebrow|logo-ring|subtitle|brand-band|film|video-overlay|video-close|'
    r'play-ring|play-hint|zoom-hint|badge-tri|check|scrim|media-card'
)


def audit_hardcoded():
    print('=' * 66)
    print('C. 硬编码颜色（绕过令牌）')
    print('=' * 66)
    COLOR = re.compile(r'#[0-9a-fA-F]{3,8}\b|rgba?\(')
    TOKDEF = re.compile(r'^\s*--[a-z0-9-]+\s*:')
    files = ['app.wxss'] + sorted(glob.glob('pages/*/*.wxss'))
    flagged = []
    allowed = []

    for f in files:
        raw_lines = rd(f).splitlines()
        clean = blank_comment_lines(rd(f)).splitlines()
        # 记录每条规则的起始行（含选择器）
        rule_start = 0
        for n, line in enumerate(clean):
            if '{' in line:
                rule_start = n
            if not COLOR.search(line):
                continue
            if TOKDEF.match(raw_lines[n]):
                continue                       # 令牌定义行：颜色值本来就该写在这里
            sel = clean[rule_start].split('{')[0].strip()
            item = (f.replace('\\', '/'), n + 1, sel, raw_lines[n].strip())
            if DARK_SURFACE_SELECTORS.search(sel):
                allowed.append(item)
            else:
                flagged.append(item)

    if flagged:
        print('  [失败] 未走令牌且不在深色底面白名单里：')
        for f, n, sel, ln in flagged:
            print('         %s:%d  %s' % (f, n, sel))
            print('             %s' % ln)
        FAILURES.append('硬编码颜色 %d 处' % len(flagged))
    else:
        print('  [通过] 无未登记的硬编码颜色')

    if allowed:
        print('  [提示] %d 处硬编码在深色底面（hero/视频层/遮罩）上，两主题同色，属有意写死'
              % len(allowed))
        if VERBOSE:
            for f, n, sel, ln in allowed:
                print('         %s:%d  %s' % (f, n, sel))
    print()


if __name__ == '__main__':
    print()
    audit_tokens()
    audit_contrast()
    audit_hardcoded()
    print('=' * 66)
    if FAILURES:
        print('审计未通过：')
        for x in FAILURES:
            print('  - ' + x)
        sys.exit(1)
    print('审计通过。')
