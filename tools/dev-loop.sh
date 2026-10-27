#!/usr/bin/env bash
# Native-only iteration loop: rebuild libot2host.so and push it into the
# installed app. Takes about 15 seconds per cycle and never installs an APK,
# which matters because MagicOS puts a security check and a lock-screen prompt
# in front of every sideload.
#
# Run the full switch2android.py build once first (it produces the APK and the
# content bundle); after that this is the whole edit-run loop.
#
# Usage: dev-loop.sh <serial> <package>
set -euo pipefail

SERIAL=${1:-}
PKG=${2:-}
[ -n "$SERIAL" ] && [ -n "$PKG" ] || { echo "usage: $0 <serial> <package>" >&2; exit 2; }

ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORKSPACE=$(cd "$ROOT/.." && pwd)
BUILD=${S2A_BUILD_DIR:-$WORKSPACE/s2a-work/android-build}
ADB=${ADB:-$(command -v adb || echo /tmp/pt/platform-tools/adb)}
export ADB_MDNS=${ADB_MDNS:-0}  # mDNS 序列号需要设为 1
export JAVA_HOME=${JAVA_HOME:-$HOME/.cmfa-toolchain/jdk}
export ANDROID_HOME=${ANDROID_HOME:-/Users/miyue/.cmfa-toolchain/android-sdk}

echo "== compiling native host"
cd "$ROOT"
./gradlew --project-cache-dir "$WORKSPACE/s2a-work/gradle-cache" \
    ":app:buildCMakeRelWithDebInfo[arm64-v8a]" \
    "-Ps2aAssets=$WORKSPACE/s2a-work/generated-assets" \
    "-Ps2aBuildDir=$BUILD" \
    "-Ps2aAppId=$PKG" \
    -Ps2aName=loop "-Ps2aVersionCode=1" --offline -q

SO=$(ls -t "$BUILD"/app/intermediates/cxx/RelWithDebInfo/*/obj/arm64-v8a/libot2host.so 2>/dev/null | head -1 || true)
[ -n "$SO" ] || { echo "build produced no library" >&2; exit 1; }

# Strip before pushing: the unstripped object is ~32 MB, the stripped one ~2.5 MB,
# and the push goes over Wi-Fi. Symbols are not needed for a bring-up run.
STRIP=$(ls "$ANDROID_HOME"/ndk/*/toolchains/llvm/prebuilt/*/bin/llvm-strip 2>/dev/null | tail -1 || true)
if [ -n "$STRIP" ]; then
    cp "$SO" "$SO.stripped"
    "$STRIP" --strip-unneeded "$SO.stripped" 2>/dev/null || cp "$SO" "$SO.stripped"
    SO=$SO.stripped
fi
echo "   $(wc -c <"$SO") bytes"

echo "== pushing"
"$ADB" -s "$SERIAL" shell am force-stop "$PKG"
"$ADB" -s "$SERIAL" shell "run-as $PKG mkdir -p files/native"
"$ADB" -s "$SERIAL" shell "run-as $PKG chmod 600 files/native/libot2host.so" 2>/dev/null || true
cat "$SO" | "$ADB" -s "$SERIAL" shell "run-as $PKG sh -c 'cat > files/native/libot2host.so'"
echo "   on device: $("$ADB" -s "$SERIAL" shell "run-as $PKG stat -c %s files/native/libot2host.so" | tr -d '\r') bytes"
# 热推库的版本戳：MainActivity 只在 versionCode 与本机安装包一致时才接受这份库，
# 否则会忽略并尽量删除它（换 APK 后自动回退到打包库，避免"新包跑旧 native"）。
# 只取 versionCode= 后面的数字：整行还有 minSdk=/targetSdk=，用 tr -dc 0-9 会把它们
# 一起粘进来（224 变成 2243336），戳对不上，MainActivity 就把刚推的库当陈旧版删掉。
# Take only the digits of versionCode=: the same line carries minSdk=/targetSdk=, and
# stripping everything but digits glued them on (224 -> 2243336), so MainActivity saw a
# stamp mismatch and deleted the library that had just been pushed.
VERSION=$("$ADB" -s "$SERIAL" shell "dumpsys package $PKG" 2>/dev/null | grep -m1 -o 'versionCode=[0-9]*' | cut -d= -f2)
if [ -n "$VERSION" ]; then
    "$ADB" -s "$SERIAL" shell "run-as $PKG sh -c 'echo $VERSION > files/native/libot2host.version'"
    echo "   stamp: versionCode=$VERSION"
fi

echo "== starting"
"$ADB" -s "$SERIAL" shell am start -n "$PKG/dev.ot2.port.LauncherActivity" >/dev/null
sleep 6
"$ADB" -s "$SERIAL" logcat -d 2>/dev/null | grep -E "OT2Native|OT2Guest" | tail -6
