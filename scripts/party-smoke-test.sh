#!/usr/bin/env bash
# HTTP smoke test for the party server.
#
# Usage: scripts/party-smoke-test.sh [path-to-party_smoke_server]
# Build the harness first with:
#   cmake -S . -B build -DCROSSFADE_BUILD_TESTS=ON
#   cmake --build build --target party_smoke_server
#
# The harness uses the real party_server.cpp with stubbed party state, so this
# test never starts the GUI, the Python backend or reads auth files.
set -euo pipefail

ROOT="$(cd "$(dirname "$0")/.." && pwd)"
BIN="${1:-$ROOT/build/party_smoke_server}"
if [ ! -x "$BIN" ]; then
    echo "missing $BIN (build target party_smoke_server first)" >&2
    exit 2
fi

PORT=$((18000 + RANDOM % 2000))
LOG="$(mktemp -t cf-party-smoke.XXXXXX)"
"$BIN" "$PORT" >"$LOG" 2>&1 &
PID=$!
cleanup() {
    kill "$PID" 2>/dev/null || true
    wait "$PID" 2>/dev/null || true
    rm -f "$LOG"
}
trap cleanup EXIT

ACTUAL=""
for _ in $(seq 1 50); do
    if grep -q '^PORT ' "$LOG" 2>/dev/null; then
        ACTUAL="$(awk '/^PORT /{print $2; exit}' "$LOG")"
        break
    fi
    sleep 0.1
done
if [ -z "$ACTUAL" ]; then
    echo "server did not start" >&2
    cat "$LOG" >&2
    exit 1
fi

BASE="http://127.0.0.1:$ACTUAL"
JAR="$(mktemp -t cf-party-jar.XXXXXX)"
HEADER_FILE="$(mktemp -t cf-party-headers.XXXXXX)"
trap 'cleanup; rm -f "$JAR" "$HEADER_FILE"' EXIT
PASS=0

fail() {
    echo "FAIL: $1" >&2
    exit 1
}

expect_code() {
    local want="$1" desc="$2"
    shift 2
    local got
    got="$(curl -sS -o /dev/null -w '%{http_code}' "$@")" || fail "$desc (curl)"
    [ "$got" = "$want" ] || fail "$desc (want $want, got $got)"
    PASS=$((PASS + 1))
}

# Health and static page.
expect_code 200 "healthz" "$BASE/healthz"
expect_code 403 "page without a guest cookie" "$BASE/"
expect_code 404 "unknown party token" "$BASE/j/wrong-token"

# Error responses must keep their explanatory bodies; the server used to blank
# every 4xx, which made a missing cookie look like a white page on a phone.
NO_COOKIE_BODY="$(curl -sS "$BASE/")"
echo "$NO_COOKIE_BODY" | grep -qi 'Scan the QR code again' || fail "403 explanation body"
PASS=$((PASS + 1))

# Join serves the guest page directly (no redirect to /) and sets an HttpOnly
# SameSite=Strict cookie in the same response. A redirect could lose the cookie
# during a cross-site QR navigation and land on a cookie-less 403.
JOIN_BODY="$(curl -sS -D "$HEADER_FILE" -c "$JAR" "$BASE/j/smoke-token")"
grep -qi '^HTTP/1.1 200' "$HEADER_FILE" || fail "join status (want 200)"
grep -qi '^Set-Cookie:.*HttpOnly' "$HEADER_FILE" || fail "cookie HttpOnly"
grep -qi '^Set-Cookie:.*SameSite=Strict' "$HEADER_FILE" || fail "cookie SameSite"
echo "$JOIN_BODY" | grep -qi 'Smoke party' || fail "join body (guest page)"
PASS=$((PASS + 4))

expect_code 200 "guest page" -b "$JAR" "$BASE/"
expect_code 200 "state" -b "$JAR" "$BASE/api/state"
expect_code 200 "unchanged state" -b "$JAR" "$BASE/api/state?v=1"

# CSRF and content-type guards.
expect_code 415 "add without X-CF" -b "$JAR" \
    -H 'Content-Type: application/json' -d '{"input":"https://youtu.be/dQw4w9WgXcQ"}' \
    "$BASE/api/add"
JSON_ERR="$(curl -sS -b "$JAR" -H 'Content-Type: application/json' \
    -d '{"input":"https://youtu.be/dQw4w9WgXcQ"}' "$BASE/api/add")"
echo "$JSON_ERR" | grep -q '"bad_request"' || fail "JSON error body"
PASS=$((PASS + 1))
expect_code 415 "add without JSON content type" -b "$JAR" -H 'X-CF: 1' \
    -d '{"input":"https://youtu.be/dQw4w9WgXcQ"}' "$BASE/api/add"
expect_code 200 "valid add" -b "$JAR" -H 'Content-Type: application/json' -H 'X-CF: 1' \
    -d '{"input":"https://youtu.be/dQw4w9WgXcQ"}' "$BASE/api/add"
expect_code 400 "invalid add" -b "$JAR" -H 'Content-Type: application/json' -H 'X-CF: 1' \
    -d '{"input":"bad"}' "$BASE/api/add"
expect_code 200 "undo" -b "$JAR" -X DELETE -H 'X-CF: 1' "$BASE/api/add/dQw4w9WgXcQ"
expect_code 200 "search" -b "$JAR" "$BASE/api/search?q=test"

# Host allowlist: only 127.0.0.1/localhost (or the bound LAN IP) is served.
expect_code 421 "foreign Host header" -H 'Host: evil.example' "$BASE/healthz"
FOREIGN_BODY="$(curl -sS -H 'Host: evil.example' "$BASE/healthz")"
echo "$FOREIGN_BODY" | grep -qi 'Open the party link' || fail "foreign Host explanation"
PASS=$((PASS + 1))

# Path traversal / bad ids never reach the filesystem.
expect_code 404 "unknown art id" -b "$JAR" "$BASE/api/art/aaaaaaaaaaa"
expect_code 404 "art traversal" -b "$JAR" "$BASE/api/art/..%2f..%2fetc%2fpasswd"
expect_code 404 "unknown route" -b "$JAR" "$BASE/api/nope"

# An art miss must stay uncached so the page can retry after the download.
MISS_HEADERS="$(curl -sS -D - -o /dev/null -b "$JAR" "$BASE/api/art/aaaaaaaaaaa")"
echo "$MISS_HEADERS" | grep -qi '^Cache-Control: no-store' || fail "art miss caching"
PASS=$((PASS + 1))

# The guest page must keep its restrictive headers.
PAGE_HEADERS="$(curl -sS -D - -o /dev/null -b "$JAR" "$BASE/")"
echo "$PAGE_HEADERS" | grep -qi '^Content-Security-Policy:' || fail "page CSP"
echo "$PAGE_HEADERS" | grep -qi '^X-Frame-Options: DENY' || fail "page frame guard"
PASS=$((PASS + 2))

echo "party smoke test: $PASS checks passed (port $ACTUAL)"
