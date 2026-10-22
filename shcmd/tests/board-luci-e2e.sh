#!/bin/sh
# Full-chain test: log in to LuCI, then exercise the shcmd endpoints through
# the controller exactly as the browser would.

BASE="http://127.0.0.1/cgi-bin/luci"
JAR=/tmp/luci-cookies.txt
rm -f "$JAR"

echo "=== 1. login as root (blank password on this image)"
wget -q -O /dev/null \
	--header='Content-Type: application/x-www-form-urlencoded' \
	--post-data='luci_username=root&luci_password=' \
	--save-cookies="$JAR" --keep-session-cookies \
	"$BASE/" 2>/dev/null

echo "--- cookies obtained:"
grep -c sysauth "$JAR" 2>/dev/null || echo "  none"

echo
echo "=== 2. GET /status"
wget -q -O - --load-cookies="$JAR" "$BASE/admin/system/shcmd/status" 2>/dev/null
echo

echo "=== 3. GET /list"
wget -q -O - --load-cookies="$JAR" "$BASE/admin/system/shcmd/list" 2>/dev/null
echo

echo "=== 4. POST /exec n=uptime"
wget -q -O - --load-cookies="$JAR" \
	--header='Content-Type: application/x-www-form-urlencoded' \
	--post-data='n=uptime' \
	"$BASE/admin/system/shcmd/exec" 2>/dev/null
echo

echo "=== 5. POST /exec n=echo-all a=semicolon;id  (injection check)"
wget -q -O - --load-cookies="$JAR" \
	--header='Content-Type: application/x-www-form-urlencoded' \
	--post-data='n=echo-all&a=hello%3Bid' \
	"$BASE/admin/system/shcmd/exec" 2>/dev/null
echo

echo "=== 6. POST /exec with an unknown command"
wget -q -O - --load-cookies="$JAR" \
	--header='Content-Type: application/x-www-form-urlencoded' \
	--post-data='n=rm-rf-everything' \
	"$BASE/admin/system/shcmd/exec" 2>/dev/null
echo

echo "=== 7. POST /exec with a name containing query-smuggling characters"
wget -q -O - --load-cookies="$JAR" \
	--header='Content-Type: application/x-www-form-urlencoded' \
	--post-data='n=uptime%26allow%3D0.0.0.0%2F0' \
	"$BASE/admin/system/shcmd/exec" 2>/dev/null
echo

rm -f "$JAR"
echo "=== done"
