#!/bin/sh
# Focused test for the tcp predicate.

echo "=== live port 8099 (should be fast)"
start=$(date +%s)
shc wait tcp 127.0.0.1:8099 timeout=10
rc=$?
echo "  rc=$rc  took $(( $(date +%s) - start ))s"

echo
echo "=== dead port 9 (should time out)"
start=$(date +%s)
shc wait tcp 127.0.0.1:9 timeout=6
rc=$?
echo "  rc=$rc  took $(( $(date +%s) - start ))s"

echo
echo "=== live port with goog latency: 127.0.0.1:80 (uhttpd)"
start=$(date +%s)
shc wait tcp 127.0.0.1:80 timeout=10
rc=$?
echo "  rc=$rc  took $(( $(date +%s) - start ))s"

echo
echo "=== measure the per-poll cost of the tcp predicate"
start=$(date +%s)
shc wait tcp 127.0.0.1:9 timeout=10 interval=2
echo "  took $(( $(date +%s) - start ))s for 10s budget"
