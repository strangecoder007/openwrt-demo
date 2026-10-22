#!/bin/sh
# The real use case: change something, then wait for it inside one command.
#
# This is why `shc wait` exists -- without it, an assistant has to fire a
# change, sleep-and-retry by hand, and burn a round trip per attempt.

echo "=== current channel:"
shc run wifi-state 2>/dev/null | grep Channel

echo
echo "############ switch to channel 6, then wait for it (one line each)"
time shc run uci-radio a=channel a1=6
echo "  run done"
time shc wait chan 6 timeout=20
echo "  wait done"

echo
echo "############ switch back to 11 and wait"
time shc run uci-radio a=channel a1=11
time shc wait chan 11 timeout=20

echo
echo "############ wait for the daemon after a restart"
time shc run restart-service a=shcmd
time shc wait service shcmd timeout=20
time shc wait tcp 127.0.0.1:8099 timeout=20

echo
echo "############ final state"
shc run wifi-state 2>/dev/null | grep -e Channel -e ESSID
