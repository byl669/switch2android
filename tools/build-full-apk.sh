#!/usr/bin/env bash
# Build a self-contained ("full") APK: the host plus the whole game.s2a, so it
# can be installed on a device that never imported the package.
#
# Difference from rebuild-apk.sh (the incremental updater):
#   rebuild-apk.sh  -> assets/{launch.json, shader-bundles.zip, shared-font.otf}
#   build-full-apk.sh -> the same three PLUS assets/game.s2a (764 MB, STORED)
#
# game.s2a is never copied: it is hard-linked into the staging directory (same
# APFS volume), so the build costs no extra source space. Android does not
# compress it either (app/build.gradle.kts: androidResources.noCompress += "s2a"),
# which is why the package lands around 770 MB.
#
# Usage: build-full-apk.sh [product-dir] [version-code] [version-name]
#   product-dir    s2a-out*/<title>-v<n> holding game.s2a   (default: s2a-out-diag)
#   version-code   default 205 (must exceed anything already on the device)
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORKSPACE=$(cd "$ROOT/.." && pwd)

PRODUCT=${1:-$WORKSPACE/s2a-out-diag/0100620012d6e000-v0}
VERSION_CODE=${2:-211}
VERSION_NAME=${3:-1.5.1}

S2A=${S2A:-$PRODUCT/game.s2a}
[ -f "$S2A" ] || { echo "game.s2a 不存在：$S2A" >&2; exit 1; }

BUNDLES=${S2A_SHADER_BUNDLES:-$WORKSPACE/s2a-work/shader-bundles.zip}
FONT=${S2A_SHARED_FONT:-$ROOT/assets/shared-font.otf}
[ -f "$BUNDLES" ] || { echo "着色器包缺失：$BUNDLES（执行 tools/pack-shader-bundles.sh）" >&2; exit 1; }
[ -f "$FONT" ]    || { echo "中文替换字体缺失：$FONT（执行 tools/make-shared-font.sh）" >&2; exit 1; }

STAGE=$WORKSPACE/s2a-work/full-assets
rm -rf "$STAGE"
mkdir -p "$STAGE"

# launch.json 必须写 embedded=true，否则装到新设备上会跳去"导入 game.s2a"。
# 优先沿用已发布完整版的描述符，保证 total_bytes/bundle_id 与前作一致。
LAUNCH_SRC=""
for candidate in $(ls -t "$WORKSPACE"/dist/BigBrainAcademy-*-full.apk 2>/dev/null); do
    LAUNCH_SRC=$candidate
    break
done
if [ -f "$LAUNCH_SRC" ] && unzip -p "$LAUNCH_SRC" assets/launch.json >"$STAGE/launch.json" 2>/dev/null; then
    echo "launch.json: 沿用 $LAUNCH_SRC"
else
    python3 - "$PRODUCT" "$STAGE/launch.json" <<'PY'
import json, sys
meta = json.load(open(sys.argv[1] + '/package.json'))
keys = ('schema', 'title_id', 'bundle_id', 'total_bytes', 'name')
out = {k: meta[k] for k in keys if k in meta}
out['embedded'] = True
json.dump(out, open(sys.argv[2], 'w'))
PY
    echo "launch.json: 由 package.json 生成"
fi
grep -q '"embedded": true' "$STAGE/launch.json" || { echo "launch.json 未标记 embedded" >&2; exit 1; }

cp "$BUNDLES" "$STAGE/shader-bundles.zip"
cp "$FONT"    "$STAGE/shared-font.otf"
# 管线预热列表的种子（可选）：新设备首启由引擎当成本机列表，第一场对战前就建好常见管线。
# 只包含着色器内容键和免指针的管线状态，不绑定 GPU；缺了也不影响启动，只是首场会边玩边编译。
SEED=${S2A_PIPELINE_SEED:-$WORKSPACE/s2a-work/seed/vk-pipeline-list.seed}
if [ -f "$SEED" ]; then
    mkdir -p "$STAGE/pipeline-seed"
    cp "$SEED" "$STAGE/pipeline-seed/vk-pipeline-list.seed"
else
    echo "提示：未找到管线种子 $SEED，本包不带预热缓存" >&2
fi
ln "$S2A" "$STAGE/game.s2a" 2>/dev/null || cp "$S2A" "$STAGE/game.s2a"

echo "product:      $PRODUCT"
echo "versionCode:  $VERSION_CODE ($VERSION_NAME)"
ls -la "$STAGE"

cd "$ROOT"
export JAVA_HOME=${JAVA_HOME:-$HOME/.cmfa-toolchain/jdk}
export ANDROID_HOME=${ANDROID_HOME:-$HOME/.cmfa-toolchain/android-sdk}
export ANDROID_SDK_ROOT=$ANDROID_HOME
NAME=$(python3 -c 'import json,sys;print(json.load(open(sys.argv[1]))["name"])' "$STAGE/launch.json")

./gradlew --offline \
    -Ps2aAssets="$STAGE" \
    -Ps2aAppId=org.switch2android.g0100620012d6e000 \
    -Ps2aName="$NAME" \
    -Ps2aVersionCode="$VERSION_CODE" \
    -Ps2aVersionName="$VERSION_NAME" \
    -Ps2aBuildDir="$ROOT/s2a-work/android-build" \
    assembleRelease

APK=$ROOT/s2a-work/android-build/app/outputs/apk/release/app-release.apk
for entry in launch.json game.s2a shader-bundles.zip shared-font.otf; do
    unzip -l "$APK" | grep -q "assets/$entry" || { echo "打包结果缺 assets/$entry" >&2; exit 1; }
done
# 预热缓存是可选的：在就检查一下条目确实进了包。
if [ -f "$STAGE/pipeline-seed/vk-pipeline-list.seed" ]; then
    unzip -l "$APK" | grep -q "assets/pipeline-seed/vk-pipeline-list.seed" || { echo "打包结果缺 assets/pipeline-seed/vk-pipeline-list.seed" >&2; exit 1; }
fi
# game.s2a 必须是 STORED，否则 764MB 会被压成 .s2a 之外的东西并让构建慢一个数量级。
unzip -v "$APK" | awk '$8=="assets/game.s2a"{print "game.s2a method:", ($2=="Stored"?"Stored (ok)":"COMPRESSED ("$2")")}'

OUT=$WORKSPACE/dist/BigBrainAcademy-$VERSION_NAME-full.apk
cp "$APK" "$OUT"
echo "==> $OUT ($(du -h "$OUT" | cut -f1))"
