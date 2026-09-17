# -*- coding: utf-8 -*-
"""对本次暗色模式改造做一次独立进程的端到端核对。

必须在与写入不同的进程里读，因为 Y: 盘（SMB）存在 read-after-write 陈旧问题：
同一进程写完立刻读回可能拿到旧内容，会误报「写入失败」。
"""
import io
import os
import re
import sys

os.chdir(os.path.dirname(os.path.dirname(os.path.abspath(__file__))))


def read(p):
    with io.open(p, encoding='utf-8') as f:
        return f.read()


fails = []


def check(cond, label, detail=''):
    print('  %s %s%s' % ('[OK]' if cond else '[!!]', label,
                         ('  ' + detail) if detail else ''))
    if not cond:
        fails.append(label)


print('1) app.wxss 结构完整性')
app = read('app.wxss')
depth = 0
for ch in re.sub(r'/\*.*?\*/', '', app, flags=re.S):
    if ch == '{':
        depth += 1
    elif ch == '}':
        depth -= 1
check(depth == 0, '花括号配平', 'depth=%d' % depth)
check(app.count('@media (prefers-color-scheme: dark)') == 1, '只有一个暗色媒体查询')
try:
    app.encode('utf-8').decode('utf-8')
    check(True, 'UTF-8 可解码')
except Exception as e:
    check(False, 'UTF-8 可解码', str(e))

print()
print('2) 令牌值（亮 / 暗）')
EXPECT = [
    ('--on-accent',     '#FFFFFF',   '#2A1B08',   '琥珀面前景：暗色改深墨字'),
    ('--on-scrim',      '#FFFFFF',   '#FFFFFF',   '遮罩面前景：主题无关'),
    ('--on-danger',     '#FFFFFF',   '#FFFFFF',   '危险面前景'),
    ('--on-brand-soft', 'rgba(255, 255, 255, 0.70)', 'rgba(255, 255, 255, 0.70)', 'hero 副标题'),
    ('--on-brand-dim',  'rgba(255, 255, 255, 0.55)', 'rgba(255, 255, 255, 0.55)', 'hero 小标签（原 0.45）'),
    ('--ink-2',         '#4F615B',   '#A7B7B2',   '次要文字（原 #5C6F6A）'),
    ('--ink-3',         '#61706B',   '#87978F',   '三级文字（原 #6D7E78）'),
    ('--accent-ink',    '#A15B13',   '#EDB57A',   '琥珀文字（原 #A95F14）'),
    ('--ok',            '#35784C',   '#6FBB8A',   '成功色（原 #3E8E5A）'),
    ('--danger',        '#B94132',   '#E48170',   '危险墨色（原 #C74636）'),
    ('--danger-fill',   '#B94132',   '#B8453A',   '危险实色面'),
    ('--accent-ring',   'rgba(217, 130, 43, 0.22)', 'rgba(224, 154, 75, 0.30)', '选中态光晕'),
]
for tok, want_light, want_dark, desc in EXPECT:
    got = [g.strip() for g in re.findall(re.escape(tok) + r'\s*:\s*([^;]+);', app)]
    ok = len(got) == 2 and got[0] == want_light and got[1] == want_dark
    check(ok, '%-16s %s' % (tok, desc),
          '亮=%s 暗=%s' % (got[0] if got else '无', got[1] if len(got) > 1 else '无'))

print()
print('3) 组件侧不再借用错角色的令牌')
CASES = [
    ('pages/detail/detail.wxss',  r'\.play-ring \.tri \{[^}]*var\(--on-scrim\)', '播放三角用 on-scrim'),
    ('pages/detail/detail.wxss',  r'\.video-close \{[^}]*var\(--on-scrim\)', '关闭按钮用 on-scrim'),
    ('pages/month/month.wxss',    r'\.video-badge \.tri \{[^}]*var\(--on-scrim\)', '视频角标用 on-scrim'),
    ('pages/month/month.wxss',    r'\.video-close \{[^}]*var\(--on-scrim\)', '关闭按钮用 on-scrim'),
    ('app.wxss',                  r'\.btn-danger \{[^}]*var\(--danger-fill\)', '删除按钮用 danger-fill'),
    ('app.wxss',                  r'\.btn-danger \{[^}]*var\(--on-danger\)', '删除按钮文字用 on-danger'),
    ('pages/month/month.wxss',    r'\.cell\.picked \{[^}]*var\(--accent-ring\)', '选中光晕用 accent-ring'),
    ('pages/login/login.wxss',    r'\.subtitle \{[^}]*var\(--on-brand-soft\)', 'login 副标题用令牌'),
    ('pages/register/register.wxss', r'\.subtitle \{[^}]*var\(--on-brand-soft\)', 'register 副标题用令牌'),
]
for path, pat, desc in CASES:
    t = read(path)
    check(re.search(pat, t, re.S) is not None, '%-34s %s' % (os.path.basename(path), desc))

print()
print('4) 旧值不应再有残留（剥离注释后扫描，注释里记录旧值属正常）')
LEFTOVER = [
    ('rgba(255, 255, 255, 0.45)', 'hero 小标签旧透明度'),
    ('rgba(255, 255, 255, 0.66)', 'hero 副标题旧透明度'),
    ('#6D7E78',                   '三级文字旧值'),
    ('#5C6F6A',                   '次要文字旧值'),
    ('#3E8E5A',                   '成功色旧值'),
    ('#C74636',                   '危险色旧值'),
]
# 注意：rgba(217,130,43,0.22) 现在是 --accent-ring 的亮色取值，#A95F14 是
# --accent-deep（渐变收尾，本次未改动），两者都不是残留，别再列进来。


def strip_comments(text):
    """去掉 /* */ 与以 * 开头的续行，避免注释里的旧值被误判为残留。"""
    text = re.sub(r'/\*.*?\*/', '', text, flags=re.S)
    return '\n'.join(l for l in text.splitlines() if not l.strip().startswith('*'))


found = 0
for f in ['app.wxss'] + ['pages/%s/%s.wxss' % (p, p) for p in
                         ('home', 'login', 'register', 'month', 'detail', 'upload')]:
    if not os.path.exists(f):
        continue
    t = strip_comments(read(f))
    for needle, desc in LEFTOVER:
        if needle in t:
            check(False, '%s 残留 %s (%s)' % (f, needle, desc))
            found += 1
if not found:
    check(True, '无旧值残留')

print()
if fails:
    print('核对未通过 %d 项：' % len(fails))
    for x in fails:
        print('  - ' + x)
    sys.exit(1)
print('核对全部通过。')
