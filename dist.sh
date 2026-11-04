#!/usr/bin/env bash
# Build a source tarball that can be copied to another Linux machine.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
VERSION="$(sed -n 's/^project(crossfade VERSION \([^ ]*\).*/\1/p' "$ROOT/CMakeLists.txt")"
OUT="${1:-$ROOT/crossfade-$VERSION-src.tar.gz}"
TMP_OUT="$(mktemp "${TMPDIR:-/tmp}/crossfade-src.XXXXXX.tar.gz")"
trap 'rm -f "$TMP_OUT"' EXIT

cd "$ROOT"
# Write outside the tree first: creating the output archive in $ROOT makes
# GNU tar warn "file changed as we read it" and exit non-zero.
tar -czf "$TMP_OUT" \
    --exclude=./build \
    --exclude=./wheels \
    --exclude=./__pycache__ \
    --exclude='*.pyc' \
    --exclude='*/.git' \
    --exclude=./external/imgui/examples \
    --exclude=./external/imgui/docs \
    --exclude=./browser.json \
    --exclude=./browseroauth.json \
    --exclude=./oauth.json \
    --exclude=./imgui.ini \
    --exclude=./p100-*-src.tar.gz \
    --exclude=./p100-data-*.tar.* \
    --transform "s,^\.,crossfade-$VERSION," \
    .

mv -f "$TMP_OUT" "$OUT"
chmod 644 "$OUT"
trap - EXIT

ls -lh "$OUT"
echo
echo "On the target machine:"
echo "  tar -xzf $(basename "$OUT") && cd crossfade-$VERSION && ./install.sh"
