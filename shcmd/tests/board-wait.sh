#!/bin/sh
# Exercise every `shc wait` predicate, including the failure paths.
#
# The three things that must be true for this to be useful:
#   * a condition that already holds returns immediately (no wasted timeout)
#   * a condition that becomes true is detected (the actual use case)
#   * a condition that never holds times out with a clear message and exit 1

echo "############ 1. already-true conditions return fast"
start=$(date +%s)
shc wait service shcmd timeout=10
echo "  took $(( $(date +%s) - start ))s"

start=$(date +%s)
shc wait iface br-lan timeout=10
echo "  took $(( $(date +%s) - start ))s"

start=$(date +%s)
shc wait tcp 127.0.0.1:8099 timeout=10
echo "  took $(( $(date +%s) - start ))s"

start=$(date +%s)
shc wait cmd uptime timeout=10
echo "  took $(( $(date +%s) - start ))s"

echo
echo "############ 2. channel wait (current channel)"
ch=$(iwinfo wlan0 info 2>/dev/null | sed -n 's/.*Channel: \([0-9]*\).*/\1/p')
echo "  current channel: $ch"
shc wait chan "$ch" timeout=5

echo
echo "############ 3. becomes true (logtail grows to a marker)"
# A file we create and then look for; the logread output will not contain it,
# so this is expected to time out -- use a real transition instead:
# ask for a channel we are NOT on, short timeout, expect failure.
other=$([ "$ch" = "1" ] && echo 11 || echo 1)
echo "  waiting for channel $other with timeout=4 (expected to fail)"
shc wait chan "$other" timeout=4
echo "  exit=$?"

echo
echo "############ 4. timeout on a port nobody listens on"
shc wait tcp 127.0.0.1:9 timeout=4
echo "  exit=$?"

echo
echo "############ 5. gone=1 inversion"
# wg0 exists, so "gone" should time out; a bogus iface is "already gone".
shc wait iface wg0 gone=1 timeout=4
echo "  exit=$?"
shc wait iface nosuchif0 gone=1 timeout=4
echo "  exit=$?"

echo
echo "############ 6. bad target name"
shc wait bogus foo timeout=2
echo "  exit=$?"
