#!/bin/sh
# Does an argument reach a `sh -c "... $1 ..."` wrapper safely?
#
# The concern: config entries like
#     command tail-log 15 sh -c 'logread | tail -n "$1"' sh
# take a caller-supplied value as $1. The value arrives as a real argv slot
# (never word-split), but the inner script still evaluates "$1" in whatever
# context its author wrote. This test shows what actually lands.
#
# Pure-shell percent-encoding: busybox awk has no sprintf("%c").

BIN=${1:-/usr/sbin/shcmd}
PORT=18300
CONF=/tmp/inj.conf

cat > "$CONF" <<'EOF'
token "T"
listen 127.0.0.1
port 18300
allow 127.0.0.1/32
command wrapped 10 sh -c 'echo "got=[$1]"' sh
command echoall 10 echo
EOF

"$BIN" -f -c "$CONF" >/tmp/inj.log 2>&1 &
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

probe() {
	label="$1"
	val="$2"
	cmd="$3"
	printf '%-26s -> ' "$label"
	wget -q -O - "http://127.0.0.1:$PORT/r?n=$cmd&k=T&a=$(enc "$val")" 2>/dev/null \
		| sed -e 's/^.*"output":"//' -e 's/","truncated".*$//'
	echo
}

echo "=== sh -c wrapper: echo \"got=[\$1]\""
probe "plain"         "hello"    wrapped
probe "semicolon"     "x; id"    wrapped
probe "command-subst" 'x$(id)'   wrapped
probe "backtick"      'x`id`'    wrapped
probe "double-quote"  'x"y'      wrapped
probe "spaces"        'a b c'    wrapped

echo
echo "=== plain argv, no wrapper (the safe default)"
probe "semicolon"     "x; id"    echoall
probe "command-subst" 'x$(id)'   echoall
probe "backtick"      'x`id`'    echoall

kill $SRV 2>/dev/null
wait $SRV 2>/dev/null
