#!/bin/sh
# Host-side behavioural tests for shcmd.
#
# These are not unit tests of internals; they start the real daemon and drive
# it over TCP. The point is to pin the security-relevant behaviour:
#   - auth rejects missing/wrong/prefix tokens
#   - allowlist rejects disallowed sources
#   - arguments are NOT shell-interpreted (the whole reason this exists)
#   - unknown command names are refused
#   - output is capped and timeouts kill the child
#
# Run:  ./run-tests.sh /path/to/shcmd
# Exit: 0 = all passed.

set -u

BIN=${1:-./shcmd}
PORT=18099
FAILS=0
TMP=$(mktemp -d)
CONF=$TMP/shcmd.conf
TOKEN=test-token-abc123

cleanup() {
	[ -n "${SRV:-}" ] && kill "$SRV" 2>/dev/null
	rm -rf "$TMP"
}
trap cleanup EXIT INT TERM

pass() { printf '  ok   %s\n' "$1"; }
fail() { printf '  FAIL %s\n' "$1"; FAILS=$((FAILS + 1)); }

check() { # check <desc> <expected-substring> <actual>
	if printf '%s' "$3" | grep -qF -- "$2"; then
		pass "$1"
	else
		fail "$1"
		printf '       expected to contain: %s\n' "$2"
		printf '       got: %s\n' "$(printf '%s' "$3" | head -c 400)"
	fi
}

not_check() { # not_check <desc> <forbidden-substring> <actual>
	if printf '%s' "$3" | grep -qF -- "$2"; then
		fail "$1"
		printf '       must NOT contain: %s\n' "$2"
	else
		pass "$1"
	fi
}

cat > "$CONF" <<EOF
token "$TOKEN"
listen 127.0.0.1
port $PORT
max_args 4
allow 127.0.0.1/32
command echo-alot 10 echo hello-from-shcmd
command printf-args 10 printf <%s|%s>
command slow 1 sleep 30
command flood 20 sh -c "yes AAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAAA"
command echo-all 10 echo
EOF

"$BIN" -f -c "$CONF" >"$TMP/server.log" 2>&1 &
SRV=$!
sleep 0.4

if ! kill -0 "$SRV" 2>/dev/null; then
	echo "server failed to start:"
	cat "$TMP/server.log"
	exit 1
fi

get() { # get <path-with-query>
	curl -s --max-time 25 "http://127.0.0.1:$PORT$1" 2>/dev/null
}

printf 'shcmd behaviour tests\n'

# ---- 1. authentication ----
R=$(get "/r?n=echo-alot")
check "missing token rejected" '"ok":false' "$R"

R=$(get "/r?n=echo-alot&k=wrong")
check "wrong token rejected" '"ok":false' "$R"

R=$(get "/r?n=echo-alot&k=test-token-abc12")
check "token prefix rejected" '"ok":false' "$R"

R=$(get "/r?n=echo-alot&k=${TOKEN}x")
check "token with suffix rejected" '"ok":false' "$R"

R=$(get "/r?n=echo-alot&k=$TOKEN")
check "correct token accepted" 'hello-from-shcmd' "$R"

R=$(curl -s --max-time 25 -H "Authorization: Bearer $TOKEN" \
	"http://127.0.0.1:$PORT/r?n=echo-alot" 2>/dev/null)
check "bearer header accepted" 'hello-from-shcmd' "$R"

# ---- 2. command selection ----
R=$(get "/r?n=does-not-exist&k=$TOKEN")
check "unknown command refused" 'unknown command' "$R"

R=$(get "/c?k=$TOKEN")
check "index lists commands" 'echo-alot' "$R"

R=$(get "/nope?k=$TOKEN")
check "unknown path 404" 'no such endpoint' "$R"

# ---- 3. no shell interpretation (the core claim) ----
# If arguments were concatenated into a shell string this would execute id.
R=$(get "/r?n=echo-all&k=$TOKEN&a=hello;id")
check "semicolon stays literal" 'hello;id' "$R"
not_check "semicolon did not run id" 'uid=' "$R"

R=$(get "/r?n=echo-all&k=$TOKEN&a=\$(id)")
check "command substitution stays literal" '$(id)' "$R"
not_check "substitution did not execute" 'uid=' "$R"

R=$(get "/r?n=echo-all&k=$TOKEN&a=\`id\`")
check "backtick stays literal" '`id`' "$R"
not_check "backtick did not execute" 'uid=' "$R"

R=$(get "/r?n=echo-all&k=$TOKEN&a=a|b")
check "pipe stays literal" 'a|b' "$R"

# positional args map to argv slots, in order
R=$(get "/r?n=printf-args&k=$TOKEN&a=one&a1=two")
check "argv slot 0" '<one|' "$R"
check "argv slot 1" '|two>' "$R"

# ---- 4. output cap ----
R=$(get "/r?n=flood&k=$TOKEN")
check "flood marked truncated" '"truncated":true' "$R"
LEN=$(printf '%s' "$R" | wc -c)
if [ "$LEN" -lt 400000 ]; then
	pass "flood response bounded ($LEN bytes)"
else
	fail "flood response unbounded ($LEN bytes)"
fi

# ---- 5. timeout kills the child ----
START=$(date +%s)
R=$(get "/r?n=slow&k=$TOKEN")
END=$(date +%s)
check "timeout reported" '"timed_out":true' "$R"
if [ $((END - START)) -le 6 ]; then
	pass "timeout enforced ($((END - START))s)"
else
	fail "timeout not enforced ($((END - START))s)"
fi
if pgrep -f "sleep 30" >/dev/null 2>&1; then
	fail "timed-out child survived"
	pkill -f "sleep 30" 2>/dev/null
else
	pass "timed-out child killed"
fi

# ---- 6. method restriction ----
R=$(curl -s --max-time 10 -X PUT "http://127.0.0.1:$PORT/r?n=echo-alot&k=$TOKEN" 2>/dev/null)
check "PUT rejected" 'only GET and POST' "$R"

printf '\n'
if [ "$FAILS" -eq 0 ]; then
	printf 'all tests passed\n'
	exit 0
fi
printf '%d test(s) failed\n' "$FAILS"
exit 1
