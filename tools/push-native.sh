#!/usr/bin/env bash
# Push the freshly linked libot2host.so into the installed app and start it.
# Same job as dev-loop.sh minus the gradle invocation, for when the library was
# already linked (gradle's CMake task can report up-to-date over a changed file).
# Usage: push-native.sh <serial> <package>
set -euo pipefail
SERIAL=${1:?serial}; PKG=${2:?package}
ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORKSPACE=$(cd "$ROOT/.." && pwd)
ADB=${ADB:-/tmp/pt/platform-tools/adb}
# /tmp is wiped on reboot, so fall back to the toolchain copy before giving up.
if [ ! -x "$ADB" ]; then
  for c in /Volumes/512SSD/Toolchains/cmfa-toolchain/android-sdk/platform-tools/adb \
           "$HOME/moveimg-toolchain/platform-tools/adb" \
           "$HOME/.cmfa-toolchain/android-sdk/platform-tools/adb"; do
    [ -x "$c" ] && { ADB=$c; break; }
  done
fi
SO=$(ls -t "$WORKSPACE"/s2a-work/android-build/app/intermediates/cxx/RelWithDebInfo/*/obj/arm64-v8a/libot2host.so | head -1)
STRIP=$(ls /Users/miyue/.cmfa-toolchain/android-sdk/ndk/*/toolchains/llvm/prebuilt/*/bin/llvm-strip 2>/dev/null | tail -1)
cp "$SO" "$SO.stripped"; "$STRIP" --strip-unneeded "$SO.stripped"
echo "so $(wc -c <"$SO.stripped") bytes"
"$ADB" -s "$SERIAL" shell am force-stop "$PKG"
"$ADB" -s "$SERIAL" shell "run-as $PKG mkdir -p files/native"
# The previous push left it read-only (the loader does not need to write it), so
# widen it before the overwrite and narrow it again afterwards.
"$ADB" -s "$SERIAL" shell "run-as $PKG chmod 600 files/native/libot2host.so" 2>/dev/null || true
cat "$SO.stripped" | "$ADB" -s "$SERIAL" shell "run-as $PKG sh -c 'cat > files/native/libot2host.so'"
"$ADB" -s "$SERIAL" shell "run-as $PKG chmod 400 files/native/libot2host.so" 2>/dev/null || true
echo "on device: $("$ADB" -s "$SERIAL" shell "run-as $PKG stat -c %s files/native/libot2host.so" | tr -d '\r')"
# 热推库的版本戳：MainActivity 只在 versionCode 与本机安装包一致时才接受这份库，
# 否则会忽略并尽量删除它（换 APK 后自动回退到打包库，避免"新包跑旧 native"）。
VERSION=$("$ADB" -s "$SERIAL" shell "dumpsys package $PKG" 2>/dev/null | grep -m1 versionCode | tr -dc '0-9')
if [ -n "$VERSION" ]; then
    "$ADB" -s "$SERIAL" shell "run-as $PKG sh -c 'echo $VERSION > files/native/libot2host.version'"
    echo "   stamp: versionCode=$VERSION"
fi
"$ADB" -s "$SERIAL" shell am start -n "$PKG/dev.ot2.port.LauncherActivity" >/dev/null
echo started
