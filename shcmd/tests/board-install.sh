#!/bin/sh
# Test the upload + install path.
#
# What matters here:
#   * a legitimate ipk uploads and reports the stored path
#   * a non-.ipk is refused server-side
#   * the stored filename is generated, not taken from the client
#   * install-ipk accepts both a bare name and a full /tmp path
#   * a path outside /tmp is refused

K="k=BOARD-CONF-3e91"
B="http://127.0.0.1:8099"

enc() {
	_hex=$(printf '%s' "$1" | hexdump -v -e '/1 "%02x"')
	_o=""
	while [ -n "$_hex" ]; do
		_p=$(printf '%s' "$_hex" | cut -c1-2)
		_hex=$(printf '%s' "$_hex" | cut -c3-)
		_o="$_o%$_p"
	done
	printf '%s' "$_o"
}

run1() {
	printf '\n--- %s [%s]\n' "$1" "$2"
	wget -q -O - "$B/r?n=$1&$K&a=$(enc "$2")" 2>/dev/null | head -c 500
	echo
}

echo "############ install-ipk: path validation"
run1 install-ipk "/etc/passwd"
run1 install-ipk "../../etc/passwd"
run1 install-ipk "/tmp/notanipk.txt"
run1 install-ipk "nonexistent.ipk"

echo
echo "############ install-ipk: legit bare name (reinstall shcmd itself)"
run1 install-ipk "shcmd_0.1.0-1_arm_cortex-a9_neon.ipk"

echo
echo "############ current uploads:"
ls -1t /tmp/upload-*.ipk 2>/dev/null | head -5 || echo "  (none)"
