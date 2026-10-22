#!/bin/sh
# Confirm the negative cases return proper 404s (busybox wget hides the body
# on non-2xx, so show status lines explicitly).

BASE="http://127.0.0.1/cgi-bin/luci"
JAR=/tmp/luci-cookies.txt
rm -f "$JAR"

wget -q -O /dev/null \
	--header='Content-Type: application/x-www-form-urlencoded' \
	--post-data='luci_username=root&luci_password=' \
	--save-cookies="$JAR" --keep-session-cookies "$BASE/" 2>/dev/null

show_status() {
	label="$1"
	shift
	printf '%-46s ' "$label"
	wget -S -O /dev/null "$@" 2>&1 | grep -o 'HTTP/1.1 [0-9]* [A-Za-z ]*' | head -1
}

show_status "unknown command" \
	--load-cookies="$JAR" \
	--header='Content-Type: application/x-www-form-urlencoded' \
	--post-data='n=rm-rf-everything' \
	"$BASE/admin/system/shcmd/exec"

show_status "command name with &smuggling" \
	--load-cookies="$JAR" \
	--header='Content-Type: application/x-www-form-urlencoded' \
	--post-data='n=uptime%26allow%3D0.0.0.0%2F0' \
	"$BASE/admin/system/shcmd/exec"

show_status "missing n" \
	--load-cookies="$JAR" \
	--header='Content-Type: application/x-www-form-urlencoded' \
	--post-data='' \
	"$BASE/admin/system/shcmd/exec"

show_status "no session (expect 403)" \
	"$BASE/admin/system/shcmd/list"

echo
echo "=== show the actual bodies for the two 4xx cases"
wget -q -O - --load-cookies="$JAR" \
	--header='Content-Type: application/x-www-form-urlencoded' \
	--post-data='n=rm-rf-everything' \
	"$BASE/admin/system/shcmd/exec" 2>/dev/null
echo
wget -q -O - --load-cookies="$JAR" \
	--header='Content-Type: application/x-www-form-urlencoded' \
	--post-data='n=uptime%26allow%3D0.0.0.0%2F0' \
	"$BASE/admin/system/shcmd/exec" 2>/dev/null
echo

rm -f "$JAR"
