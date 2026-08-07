#!/usr/bin/env bash
# Verify the shipped macOS DMG, including a real Electron startup.
set -euo pipefail

DMG="${1:?usage: verify-macos-desktop-dmg.sh <dmg> <arm64|amd64>}"
ARCH="${2:?usage: verify-macos-desktop-dmg.sh <dmg> <arm64|amd64>}"

case "$ARCH" in
    arm64) FILE_ARCH="arm64" ;;
    amd64) FILE_ARCH="x86_64" ;;
    *)
        echo "verify-macos-desktop-dmg: unsupported architecture: $ARCH" >&2
        exit 2
        ;;
esac

if [ "$(uname -s)" != "Darwin" ]; then
    echo "verify-macos-desktop-dmg: macOS is required" >&2
    exit 2
fi
if [ ! -f "$DMG" ]; then
    echo "verify-macos-desktop-dmg: missing DMG: $DMG" >&2
    exit 2
fi

MOUNT_DIR=$(mktemp -d "/tmp/cbm-desktop-mount.XXXXXX")
TEST_ROOT=$(mktemp -d "/tmp/cbm-desktop-smoke.XXXXXX")
DESKTOP_PID=""

cleanup() {
    if [ -n "$DESKTOP_PID" ] && kill -0 "$DESKTOP_PID" 2>/dev/null; then
        kill "$DESKTOP_PID" 2>/dev/null || true
        wait "$DESKTOP_PID" 2>/dev/null || true
    fi
    hdiutil detach "$MOUNT_DIR" -quiet 2>/dev/null || true
    rmdir "$MOUNT_DIR" 2>/dev/null || true
    rm -rf -- "$TEST_ROOT"
}
trap cleanup EXIT
trap 'exit 130' INT
trap 'exit 143' TERM

hdiutil attach "$DMG" -nobrowse -readonly -mountpoint "$MOUNT_DIR" -quiet
APP_COUNT=$(find "$MOUNT_DIR" -maxdepth 1 -type d -name '*.app' | wc -l | tr -d ' ')
if [ "$APP_COUNT" -ne 1 ]; then
    echo "verify-macos-desktop-dmg: expected one application bundle, found $APP_COUNT" >&2
    find "$MOUNT_DIR" -maxdepth 1 -type d -name '*.app' -print >&2
    exit 1
fi
APP=$(find "$MOUNT_DIR" -maxdepth 1 -type d -name '*.app' -print -quit)

INFO_PLIST="$APP/Contents/Info.plist"
BUNDLE_NAME=$(/usr/libexec/PlistBuddy -c 'Print :CFBundleName' "$INFO_PLIST")
APP_NAME=$(basename "$APP" .app)
if [ "$BUNDLE_NAME" != "$APP_NAME" ]; then
    echo "verify-macos-desktop-dmg: CFBundleName '$BUNDLE_NAME' does not match app '$APP_NAME'" >&2
    exit 1
fi

DESKTOP_BINARY="$APP/Contents/MacOS/$APP_NAME"
HELPER_BINARY="$APP/Contents/Frameworks/$BUNDLE_NAME Helper.app/Contents/MacOS/$BUNDLE_NAME Helper"
MCP_BINARY="$APP/Contents/Resources/bin/codebase-memory-mcp"

codesign --verify --deep --strict "$APP"
test -x "$DESKTOP_BINARY"
test -x "$HELPER_BINARY"
test -x "$MCP_BINARY"
test -f "$APP/Contents/Resources/LICENSE"
test -f "$APP/Contents/Resources/THIRD_PARTY_NOTICES.md"
file "$DESKTOP_BINARY" | grep -q "$FILE_ARCH"
file "$HELPER_BINARY" | grep -q "$FILE_ARCH"
file "$MCP_BINARY" | grep -q "$FILE_ARCH"
"$MCP_BINARY" --version

mkdir -p \
    "$TEST_ROOT/home" \
    "$TEST_ROOT/tmp" \
    "$TEST_ROOT/cache" \
    "$TEST_ROOT/user-data"
STARTUP_LOG="$TEST_ROOT/desktop.log"

env \
    HOME="$TEST_ROOT/home" \
    USERPROFILE="$TEST_ROOT/home" \
    TEMP="$TEST_ROOT/tmp" \
    TMP="$TEST_ROOT/tmp" \
    TMPDIR="$TEST_ROOT/tmp" \
    CBM_CACHE_DIR="$TEST_ROOT/cache" \
    ELECTRON_ENABLE_LOGGING=1 \
    "$DESKTOP_BINARY" \
        --user-data-dir="$TEST_ROOT/user-data" \
        --disable-gpu \
        >"$STARTUP_LOG" 2>&1 &
DESKTOP_PID=$!

ATTEMPT=0
while [ "$ATTEMPT" -lt 12 ]; do
    sleep 0.25
    if ! kill -0 "$DESKTOP_PID" 2>/dev/null; then
        STATUS=0
        wait "$DESKTOP_PID" || STATUS=$?
        DESKTOP_PID=""
        cat "$STARTUP_LOG" >&2
        echo "verify-macos-desktop-dmg: desktop exited during startup (status $STATUS)" >&2
        exit 1
    fi
    ATTEMPT=$((ATTEMPT + 1))
done

if ! pgrep -P "$DESKTOP_PID" -f "$BUNDLE_NAME Helper" >/dev/null; then
    cat "$STARTUP_LOG" >&2
    echo "verify-macos-desktop-dmg: Electron did not start a helper process" >&2
    exit 1
fi

kill "$DESKTOP_PID"
wait "$DESKTOP_PID" 2>/dev/null || true
DESKTOP_PID=""
echo "macOS desktop DMG verified: $DMG ($ARCH)"
