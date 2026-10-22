#!/bin/sh
# Exercise the real, useful commands against the live daemon.

K="k=BOARD-CONF-3e91"
B="http://127.0.0.1:8099"
J=/tmp/cookies.txt
rm -f "$J"

q() {
	printf '\n--- %s\n' "$1"
	wget -q -O - "$B/r?n=$2&$K${3:+&a=$3}" 2>/dev/null | head -c 700
	echo
}

echo "############ read-only"
q "uptime" uptime
q "wifi-clients (who is on the AP)" wifi-clients

echo
echo "############ parameterised"
q "logtail 5" logtail 5
q "traffic-period week" traffic-period week

echo
echo "############ destructive (safe choices only)"
q "sdcard status" sdcard status
q "set-txpower 15 (current value, no change in effect)" set-txpower 15

echo
echo "############ argument validation by the inner script"
q "restart-service bogus (should be refused by the case statement)" restart-service bogus
q "sdcard bogus (should be refused)" sdcard bogus

echo
echo "############ injection attempt through a {a1} placeholder"
q "logtail '1; id' (semicolon must stay literal)" logtail '1; id'
q "logtail '1 \$(id)'" logtail '1$(id)'

echo
echo "############ did anything execute?"
R=$(wget -q -O - "$B/r?n=logtail&$K&a=1%3B%20id" 2>/dev/null)
case "$R" in
	*uid=*) echo "  *** INJECTION SUCCEEDED ***" ;;
	*)      echo "  no uid= in output: OK" ;;
esac
