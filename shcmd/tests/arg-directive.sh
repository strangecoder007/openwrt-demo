#!/bin/sh
# Why is `arg 1 "label" "example" "expect"` reported as unknown?
# Feed progressively simpler forms and see which are accepted.

BIN=${1:-/usr/sbin/shcmd}

try() {
	printf '%s\n' "$1" > /tmp/t1.conf
	printf 'token "T"\nlisten 127.0.0.1\nport 18450\nallow 127.0.0.1/32\ncommand x 5 echo\n' > /tmp/t1.conf
	printf '%s\n' "$1" >> /tmp/t1.conf
	out=$("$BIN" -f -c /tmp/t1.conf 2>&1 &
	      sleep 1; kill %1 2>/dev/null)
	printf '%-62s => %s\n' "$1" "$(printf '%s' "$out" | grep -o 'unknown directive.*' || echo ACCEPTED)"
}

echo "=== arg variants"
try 'arg 1 "Label" "ex"'
try 'arg 1 "Label" "ex" "expect"'
try 'arg 1 Label ex'
try 'arg 1 "Filename in /tmp" "a.ipk" "*.ipk"'
try 'arg 1 "Label with space" "value"'

echo
echo "=== the exact line from the shipped config"
try 'arg 1 "Filename in /tmp" "apptraffic_1.0.0-24_arm_cortex-a9_neon.ipk" "*.ipk already in /tmp"'

echo
echo "=== token count probe: does tok[] have room?"
try 'arg 1 a b c d e f g h i j k l m n o p q r s t u v'
