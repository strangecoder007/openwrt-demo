#!/bin/sh
# 在构建主机(host gcc)编译并运行所有 host 单元测试。
set -e
SRC="$(cd "$(dirname "$0")/../src" && pwd)"
OUT=/tmp/wp-tests
mkdir -p "$OUT"

# smoke: 仅验证头文件+版本宏
gcc -std=gnu99 -Wall -I"$SRC" "$(dirname "$0")/smoke.c" -o "$OUT/smoke"
"$OUT/smoke"
echo "smoke OK"

# radiotap: RSSI/freq/rate 解析
gcc -std=gnu99 -Wall -I"$SRC" "$(dirname "$0")/radiotap_test.c" "$SRC/radiotap.c" -o "$OUT/radiotap_test"
"$OUT/radiotap_test"
echo "radiotap OK"

# ieee80211: MAC 帧头解析
gcc -std=gnu99 -Wall -I"$SRC" "$(dirname "$0")/ieee80211_test.c" "$SRC/ieee80211.c" -o "$OUT/ieee80211_test"
"$OUT/ieee80211_test"
echo "ieee80211 OK"

# probe: Probe Request/Beacon body 解析
gcc -std=gnu99 -Wall -I"$SRC" "$(dirname "$0")/probe_test.c" "$SRC/probe.c" -o "$OUT/probe_test"
"$OUT/probe_test"
echo "probe OK"

# device: 设备聚合 + MAC 匿名化 + 会话
gcc -std=gnu99 -Wall -I"$SRC" "$(dirname "$0")/device_test.c" "$SRC/device.c" -o "$OUT/device_test"
"$OUT/device_test"
echo "device OK"

# frame: radiotap->802.11->probe->device 全链路
gcc -std=gnu99 -Wall -I"$SRC" "$(dirname "$0")/frame_test.c" "$SRC/frame.c" "$SRC/radiotap.c" "$SRC/ieee80211.c" "$SRC/probe.c" "$SRC/device.c" -o "$OUT/frame_test"
"$OUT/frame_test"
echo "frame OK"

# db: SQLite 存储/查询 (:memory:)
ROOT="$(cd "$(dirname "$0")/../../../../.." && pwd)"
SQLITE3_H="$(find "$ROOT/staging_dir" -name sqlite3.h -path "*usr/include/*" 2>/dev/null | head -1)"
if [ -f /usr/include/sqlite3.h ]; then
  gcc -std=gnu99 -Wall -I"$SRC" "$(dirname "$0")/db_test.c" "$SRC/db.c" -lsqlite3 -o "$OUT/db_test"
elif [ -n "$SQLITE3_H" ]; then
  gcc -std=gnu99 -Wall -I"$(dirname "$SQLITE3_H")" -I"$SRC" "$(dirname "$0")/db_test.c" "$SRC/db.c" -l:libsqlite3.so.0 -o "$OUT/db_test"
else
  echo "db test skipped: no sqlite3.h available"
  exit 0
fi
"$OUT/db_test"
echo "db OK"

# cli: 参数解析
gcc -std=gnu99 -Wall -I"$SRC" "$(dirname "$0")/cli_test.c" "$SRC/args.c" -o "$OUT/cli_test"
"$OUT/cli_test"
echo "cli OK"
