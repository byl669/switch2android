#!/usr/bin/env bash
# Push a freshly built libot2host.so into an installed app, skipping the APK
# install. MagicOS gates sideloads behind a security check and a lock-screen
# prompt, so a native-only change should never need a reinstall.
#
# The app loads <files>/native/libot2host.so when present (see MainActivity
# .loadNativeHost) and falls back to the packaged library otherwise, so the
# first install has to happen once; after that this script is the whole loop.
#
# Usage: hot-push.sh <serial> <package> [apk-path]
set -euo pipefail

SERIAL=${1:-}
PKG=${2:-}
APK=${3:-}

[ -n "$SERIAL" ] && [ -n "$PKG" ] || {
    echo "usage: $0 <serial> <package> [apk-path]" >&2
    exit 2
}

ROOT=$(cd "$(dirname "$0")/.." && pwd)          # the switch2android checkout
WORKSPACE=$(cd "$ROOT/.." && pwd)               # holds s2a-out-diag/
ADB=${ADB:-/tmp/pt/platform-tools/adb}
export ADB_MDNS=0

if [ -z "$APK" ]; then
    APK=$(ls -t "$WORKSPACE"/s2a-out-diag/*/game.apk 2>/dev/null | head -1 || true)
    [ -n "$APK" ] || APK=$(ls -t "$ROOT"/../s2a-out/*/game.apk 2>/dev/null | head -1 || true)
fi
[ -n "$APK" ] && [ -f "$APK" ] || { echo "APK not found; pass one explicitly" >&2; exit 1; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

echo "== extracting native library from $(basename "$APK")"
unzip -o -q "$APK" 'lib/arm64-v8a/libot2host.so' -d "$WORK"
SO="$WORK/lib/arm64-v8a/libot2host.so"
[ -f "$SO" ] || { echo "libot2host.so missing from APK" >&2; exit 1; }
echo "   $(wc -c <"$SO") bytes"

echo "== stopping $PKG"
"$ADB" -s "$SERIAL" shell am force-stop "$PKG"

echo "== writing to app files dir"
"$ADB" -s "$SERIAL" shell "run-as $PKG mkdir -p files/native"
# A previous push leaves the file read-only so the runtime accepts it as code;
# the owner can always chmod it back before overwriting.
"$ADB" -s "$SERIAL" shell "run-as $PKG chmod 600 files/native/libot2host.so" 2>/dev/null || true
cat "$SO" | "$ADB" -s "$SERIAL" shell "run-as $PKG sh -c 'cat > files/native/libot2host.so'"

SIZE=$("$ADB" -s "$SERIAL" shell "run-as $PKG stat -c %s files/native/libot2host.so" | tr -d '\r')
# 热推库的版本戳：MainActivity 只在 versionCode 与本机安装包一致时才接受这份库，
# 否则会忽略并尽量删除它（换 APK 后自动回退到打包库，避免"新包跑旧 native"）。
VERSION=$("$ADB" -s "$SERIAL" shell "dumpsys package $PKG" 2>/dev/null | grep -m1 versionCode | tr -dc '0-9')
if [ -n "$VERSION" ]; then
    "$ADB" -s "$SERIAL" shell "run-as $PKG sh -c 'echo $VERSION > files/native/libot2host.version'"
    echo "   stamp: versionCode=$VERSION"
fi
echo "   on device: $SIZE bytes"

echo "== starting $PKG"
"$ADB" -s "$SERIAL" shell am start -n "$PKG/dev.ot2.port.LauncherActivity" >/dev/null
sleep 8
"$ADB" -s "$SERIAL" logcat -d 2>/dev/null | grep "OT2Native" | tail -2
