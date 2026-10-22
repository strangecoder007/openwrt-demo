#!/bin/sh
# Test the two newest commands: uci-radio and tcpdump.
# Deliberately exercises the refusal paths too, since those are what keep a
# caller from writing arbitrary config keys.

K="k=BOARD-CONF-3e91"
B="http://127.0.0.1:8099"

# Pure-shell percent-encoding (busybox awk has no sprintf("%c")).
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

run() {
	printf '\n--- %s  [%s | %s]\n' "$1" "$2" "$3"
	wget -q -O - "$B/r?n=$1&$K&a=$(enc "$2")&a1=$(enc "$3")" 2>/dev/null | head -c 600
	echo
}

one() {
	printf '\n--- %s\n' "$1"
	wget -q -O - "$B/r?n=$1&$K" 2>/dev/null | head -c 600
	echo
}

echo "############ wifi-state (before)"
one wifi-state

echo
echo "############ uci-radio: refusals"
run uci-radio "channel" "7"
run uci-radio "txpower" "99"
run uci-radio "htmode" "HT160"
run uci-radio "password" "hunter2"

echo
echo "############ uci-radio: injection attempts"
run uci-radio "channel" "1; reboot"
run uci-radio "txpower" "15; id"

echo
echo "############ uci-radio: real (channel 11 = current value)"
run uci-radio "channel" "11"

echo
echo "############ tcpdump: refusals"
run tcpdump "tun0" "10"
run tcpdump "any" "99999"
run tcpdump "any" "abc"

echo
echo "############ tcpdump: real capture"
run tcpdump "any" "20"

echo
echo "############ leftover capture files:"
ls -l /tmp/cap-*.pcap 2>/dev/null || echo "  (none)"
