#!/bin/sh
# Verify the LuCI layer: the /list and /status endpoints do not require a
# command run, and they tell us whether the controller can read the config.

BASE="http://127.0.0.1/cgi-bin/luci/admin/system/shcmd"

echo "=== kill stray test instance"
for p in /proc/[0-9]*; do
	c=$(cat "$p/comm" 2>/dev/null)
	if [ "$c" = "shcmd" ]; then
		cmd=$(tr '\0' ' ' < "$p/cmdline")
		case "$cmd" in
		*/tmp/c.conf*|*/tmp/t.conf*)
			echo "  killing ${p#/proc/} ($cmd)"
			kill -9 "${p#/proc/}" 2>/dev/null
			;;
		esac
	fi
done

echo "=== does the controller route exist? (expect 403/401, not 404)"
wget -S -O /dev/null "$BASE/status" 2>&1 | grep -e 'HTTP/' -e 'Location'

echo
echo "=== luci cookie-less request to /list:"
wget -S -O - "$BASE/list" 2>&1 | grep -e 'HTTP/' -e 'ok' | head -5

echo
echo "=== is the menu entry registered in the index cache?"
ls /tmp/luci-indexcache* 2>/dev/null || echo "  (no cache file yet)"

echo
echo "=== does luci see the controller file?"
lua -e 'package.path=package.path..";/usr/lib/lua/?.lua";' 2>/dev/null
ls -l /usr/lib/lua/luci/controller/shcmd.lua
