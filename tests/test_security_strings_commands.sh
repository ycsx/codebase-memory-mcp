#!/usr/bin/env bash
# Regression guard for dangerous-command matching in binary string audits.
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")/.." && pwd)"
SCRIPT="$ROOT/scripts/security-strings.sh"

TMP="$(mktemp -d)"
trap 'rm -rf "$TMP"' EXIT

make_fixture() {
    local out="$1"
    shift
    printf '\x00\x01\x02\x03\x04\x05\x06\x07\xff\xfe\xfd\xfc' > "$out"
    local value
    for value in "$@"; do
        printf '%s\x00' "$value" >> "$out"
    done
    printf '\x00\x00\x00\x00\xff\xff\xff\xff' >> "$out"
}

PASS=0
FAIL=0

expect_allowed() {
    local label="$1"
    local value="$2"
    local fixture="$TMP/allowed-$PASS-$FAIL.bin"
    make_fixture "$fixture" "$value"
    if bash "$SCRIPT" "$fixture" >/dev/null 2>&1; then
        echo "PASS: $label is not treated as a command"
        PASS=$((PASS + 1))
    else
        echo "FAIL: $label was blocked"
        bash "$SCRIPT" "$fixture" 2>&1 | grep -i "BLOCKED" || true
        FAIL=$((FAIL + 1))
    fi
}

expect_blocked() {
    local label="$1"
    local value="$2"
    local fixture="$TMP/blocked-$PASS-$FAIL.bin"
    make_fixture "$fixture" "$value"
    if bash "$SCRIPT" "$fixture" >/dev/null 2>&1; then
        echo "FAIL: $label was not blocked"
        FAIL=$((FAIL + 1))
    else
        echo "PASS: $label is still blocked"
        PASS=$((PASS + 1))
    fi
}

# Installer compression tables can contain these inert fragments by chance.
expect_allowed "hyphen-prefixed ncat fragment" "X-ncat"
expect_allowed "hyphenated ncat token" "payload-ncat-table"
expect_allowed "embedded ncat token" "concatenate"

# Real executable and shell/path forms must remain blocking.
expect_blocked "standalone ncat command" "ncat --listen 4444"
expect_blocked "Windows ncat executable" "ncat.exe --listen 4444"
expect_blocked "path-qualified ncat executable" "/usr/bin/ncat --listen 4444"
expect_blocked "Windows wget executable" "wget.exe --output-document payload.bin"
expect_blocked "shell TCP redirection" "bash -c >/dev/tcp/127.0.0.1/4444"

echo "=== security-strings command test: $PASS passed, $FAIL failed ==="
[ "$FAIL" -eq 0 ]
