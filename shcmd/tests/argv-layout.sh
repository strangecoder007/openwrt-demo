#!/bin/sh
# Nail down the exact argv layout when a config entry uses `sh -c '...' sh`.

BIN=${1:-/usr/sbin/shcmd}
PORT=18301
CONF=/tmp/argv.conf

cat > "$CONF" <<'EOF'
token "T"
listen 127.0.0.1
port 18301
allow 127.0.0.1/32
command showargv 10 sh -c 'echo "0=[$0] 1=[$1] 2=[$2] count=$#"'
command showargv-sh 10 sh -c 'echo "0=[$0] 1=[$1] 2=[$2] count=$#"' sh
EOF

"$BIN" -f -c "$CONF" >/tmp/argv.log 2>&1 &
SRV=$!
sleep 1

echo "=== entry WITHOUT the trailing 'sh'  (argv: sh -c 'script')"
wget -q -O - "http://127.0.0.1:$PORT/r?n=showargv&k=T&a=foo" 2>/dev/null
echo
wget -q -O - "http://127.0.0.1:$PORT/r?n=showargv&k=T&a=foo&a1=bar" 2>/dev/null
echo

echo "=== entry WITH the trailing 'sh'     (argv: sh -c 'script' sh)"
wget -q -O - "http://127.0.0.1:$PORT/r?n=showargv-sh&k=T&a=foo" 2>/dev/null
echo
wget -q -O - "http://127.0.0.1:$PORT/r?n=showargv-sh&k=T&a=foo&a1=bar" 2>/dev/null
echo

kill $SRV 2>/dev/null
wait $SRV 2>/dev/null
