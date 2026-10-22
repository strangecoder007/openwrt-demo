#!/bin/sh
# Placeholder substitution and shell-quoting tests.
#
# The point: a caller-supplied value must be unable to break out of the
# single-quoted substitution that {saN} produces, and must not be re-split or
# re-evaluated when it lands inside a `sh -c` script.

BIN=${1:-/usr/sbin/shcmd}
PORT=18400
CONF=/tmp/ph.conf

cat > "$CONF" <<'EOF'
token "T"
listen 127.0.0.1
port 18400
allow 127.0.0.1/32
command raw 10 sh -c 'printf "<%s>\n" {a1}'
command quoted 10 sh -c 'printf "<%s>\n" {sa1}'
command append 10 echo PRE
command twice 10 sh -c 'echo one={sa1} two={sa2}'
EOF

"$BIN" -f -c "$CONF" >/tmp/ph.log 2>&1 &
SRV=$!
sleep 1

enc() {
	_hex=$(printf '%s' "$1" | hexdump -v -e '/1 "%02x"')
	_out=""
	while [ -n "$_hex" ]; do
		_pair=$(printf '%s' "$_hex" | cut -c1-2)
		_hex=$(printf '%s' "$_hex" | cut -c3-)
		_out="$_out%$_pair"
	done
	printf '%s' "$_out"
}

out() {
	wget -q -O - "http://127.0.0.1:$PORT/r?n=$1&k=T&a=$(enc "$2")${3:+&a1=$(enc "$3")}" 2>/dev/null \
		| sed -e 's/^.*"output":"//' -e 's/","truncated".*$//'
}

echo "=== {a1} raw substitution (no quoting)"
printf '  plain      : %s\n' "$(out raw 'hello')"
printf '  spaces     : %s\n' "$(out raw 'a b c')"

echo
echo "=== {sa1} quoted substitution (must stay inert)"
printf '  plain      : %s\n' "$(out quoted 'hello')"
printf '  spaces     : %s\n' "$(out quoted 'a b c')"
printf '  semicolon  : %s\n' "$(out quoted 'x; id')"
printf '  subst      : %s\n' "$(out quoted 'x$(id)')"
printf '  backtick   : %s\n' "$(out quoted 'x`id`')"
printf '  singlequote: %s\n' "$(out quoted "x'y")"
printf '  quote-close: %s\n' "$(out quoted "'; id; echo '")"
printf '  newline    : %s\n' "$(out quoted 'a
b')"

echo
echo "=== append mode (no placeholder)"
printf '  semicolon  : %s\n' "$(out append 'x; id')"
printf '  subst      : %s\n' "$(out append 'x$(id)')"

echo
echo "=== two placeholders in one script"
printf '  both       : %s\n' "$(out twice 'one' 'two')"

echo
echo "=== did anything actually execute? (uid= would mean yes)"
RES=$(out quoted "'; id; echo '")
case "$RES" in
	*uid=*) echo "  *** INJECTION SUCCEEDED ***" ;;
	*)      echo "  no uid= in output: OK" ;;
esac

kill $SRV 2>/dev/null
wait $SRV 2>/dev/null
