#!/usr/bin/env bash
# 用 playwright-cli 抓取预览页的浅色 / 深色两套截图，供人工复核。
# 用法： bash preview/_shoot.sh [输出目录]
# 默认 ../_artifacts/qa（刻意放在 wechat-miniprogram/ 之外：截图会撑爆 2MB 代码包）
set -u

cd "$(dirname "$0")/.."
URL="http://127.0.0.1:8791/index.html"
# 默认输出到小程序目录之外 —— 截图是几十~上百 KB 的 PNG，落在
# wechat-miniprogram/ 里会把代码包撑过 2MB，导致真机调试上传失败。
OUT="${1:-../_artifacts/qa}"
S="-s=shot"
mkdir -p "$OUT"

# 本机适配：
#  1) playwright-cli 的浏览器守护进程环境里没有 ProgramFiles，会去找
#     "undefined\Program Files\Google\Chrome\Application\chrome.exe" 而报
#     「Chromium distribution 'chrome' is not found」。显式补上即可。
#  2) 会话会因空闲被回收，每次都要自己 open，不能依赖上次调用留下的会话。
export PROGRAMFILES="C:\\Program Files"
export ProgramFiles="C:\\Program Files"

playwright-cli $S close >/dev/null 2>&1
echo "-- 打开浏览器"
playwright-cli $S open "$URL" --browser chrome 2>&1 | grep -iE "Page Title|error" | head -3
playwright-cli $S resize 1500 1200 >/dev/null 2>&1

hide_topbar() {
  playwright-cli $S eval "() => { var t=document.querySelector('.topbar'); if(t) t.style.display='none'; return 1 }" >/dev/null 2>&1
}

shot() {   # shot <元素选择器> <输出文件名>
  local out
  out=$(playwright-cli $S screenshot "$1" --filename "$OUT/$2" 2>&1)
  if [ -f "$OUT/$2" ]; then
    printf '  %-24s %s 字节\n' "$2" "$(wc -c < "$OUT/$2" | tr -d ' ')"
  else
    printf '  %-24s 失败: %s\n' "$2" "$(echo "$out" | head -2 | tr '\n' ' ')"
  fi
}

PAGES="new-home new-month new-detail new-detail-closed new-upload new-login new-register"

echo "-- 浅色"
playwright-cli $S eval "() => { showTab('sec-new'); return 1 }" >/dev/null 2>&1
hide_topbar; sleep 1
for id in $PAGES; do shot "#$id" "light-${id#new-}.png"; done
playwright-cli $S eval "() => { showTab('sec-states'); return 1 }" >/dev/null 2>&1
hide_topbar; sleep 1
shot "#states" "light-states.png"

echo "-- 深色"
playwright-cli $S eval "() => { applyTheme('dark'); return 1 }" >/dev/null 2>&1
sleep 3
playwright-cli $S eval "() => { showTab('sec-new'); return 1 }" >/dev/null 2>&1
hide_topbar; sleep 1
for id in $PAGES; do shot "#$id" "dark-${id#new-}.png"; done
playwright-cli $S eval "() => { showTab('sec-states'); return 1 }" >/dev/null 2>&1
hide_topbar; sleep 1
shot "#states" "dark-states.png"

playwright-cli $S close >/dev/null 2>&1
echo "-- 完成，产物在 $OUT"
