#!/usr/bin/env bash
# Strip a staged release binary without mutating the build output.
set -euo pipefail

BINARY="${1:?usage: strip-release-binary.sh <binary> <platform>}"
PLATFORM="${2:?usage: strip-release-binary.sh <binary> <platform>}"

if [ ! -f "$BINARY" ]; then
    echo "strip-release-binary: missing binary: $BINARY" >&2
    exit 2
fi

BEFORE_BYTES=$(wc -c < "$BINARY" | tr -d ' ')
STRIPPED_WITH=""

for TOOL in "${STRIP:-}" llvm-strip strip; do
    [ -n "$TOOL" ] || continue
    command -v "$TOOL" >/dev/null 2>&1 || continue

    if "$TOOL" --strip-all "$BINARY" 2>/dev/null; then
        STRIPPED_WITH="$TOOL --strip-all"
    elif [ "$PLATFORM" = "darwin" ] && "$TOOL" "$BINARY" 2>/dev/null; then
        # Apple's strip has no --strip-all; plain strip removes the full symbol table.
        STRIPPED_WITH="$TOOL"
    fi
    [ -n "$STRIPPED_WITH" ] && break
done

if [ -z "$STRIPPED_WITH" ]; then
    echo "strip-release-binary: no working strip tool for $BINARY" >&2
    exit 2
fi

if [ "$PLATFORM" = "darwin" ]; then
    command -v codesign >/dev/null 2>&1 || {
        echo "strip-release-binary: codesign is required for macOS" >&2
        exit 2
    }
    codesign --sign - --force "$BINARY"
    codesign --verify --strict "$BINARY"
fi

AFTER_BYTES=$(wc -c < "$BINARY" | tr -d ' ')
echo "stripped $(basename "$BINARY") with $STRIPPED_WITH ($BEFORE_BYTES -> $AFTER_BYTES bytes)"
