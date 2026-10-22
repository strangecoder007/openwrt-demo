#!/bin/sh
# End-to-end upload test through LuCI.
#
# Uses wget's --post-file trick: build a multipart body by hand so we control
# the boundary and the filename exactly. That lets us test hostile filenames
# and the extension gate.

BASE="http://127.0.0.1/cgi-bin/luci"
JAR=/tmp/up-cookies.txt
BOUND="----shcmdtest1234"
BODY=/tmp/up-body.bin

rm -f "$JAR"

echo "=== login"
wget -q -O /dev/null \
	--header='Content-Type: application/x-www-form-urlencoded' \
	--post-data='luci_username=root&luci_password=' \
	--save-cookies="$JAR" --keep-session-cookies "$BASE/" 2>/dev/null
grep -c sysauth "$JAR" >/dev/null 2>&1 && echo "  ok" || echo "  NO COOKIE"

upload() {
	label="$1"
	filename="$2"
	content="$3"

	# Build multipart body: headers + payload + closing boundary.
	{
		printf -- '--%s\r\n' "$BOUND"
		printf 'Content-Disposition: form-data; name="file"; filename="%s"\r\n' "$filename"
		printf 'Content-Type: application/octet-stream\r\n\r\n'
		printf '%s' "$content"
		printf '\r\n--%s--\r\n' "$BOUND"
	} > "$BODY"

	printf '\n--- %s\n' "$label"
	wget -q -O - \
		--load-cookies="$JAR" \
		--header="Content-Type: multipart/form-data; boundary=$BOUND" \
		--post-file="$BODY" \
		"$BASE/admin/system/shcmd/upload" 2>/dev/null | head -c 400
	echo
}

echo
echo "=== reject: wrong extension"
upload "a shell script" "evil.sh" '#!/bin/sh
echo pwned'

echo
echo "=== reject: no extension"
upload "extensionless" "payload" 'data'

echo
echo "=== traversal attempt in the filename"
upload "path traversal" "../../etc/passwd.ipk" 'not a real ipk'

echo
echo "=== legitimate ipk content"
upload "real-ish ipk" "my-package.ipk" 'THIS-IS-A-FAKE-IPK-PAYLOAD'

echo
echo "=== what landed in /tmp:"
ls -l /tmp/upload-*.ipk 2>/dev/null || echo "  (none)"

echo
echo "=== content of the stored file (must equal what we sent):"
for f in /tmp/upload-*.ipk; do
	[ -e "$f" ] || continue
	echo "  $f:"
	head -c 60 "$f"
	echo
done

echo
echo "=== uploads endpoint:"
wget -q -O - --load-cookies="$JAR" "$BASE/admin/system/shcmd/installed" 2>/dev/null
echo

rm -f "$JAR" "$BODY"
