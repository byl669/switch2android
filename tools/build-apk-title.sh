#!/usr/bin/env bash
# Build a distributable full APK for any title, from a product directory that
# already holds game.s2a (tools/make-s2a.py).
#
# Difference from build-full-apk.sh (Big Brain Academy, hard-coded app id and
# output name): everything identifying the title is an argument here.
#
# Usage: build-apk-title.sh <product-dir> <title-id> <version-code> <version-name> [label]
#   product-dir    holds game.s2a
#   title-id       16 hex chars, e.g. 01001c500e72e000
#   label          桌面显示名；默认沿用游戏的英文名
# Env:
#   S2A_SHADER_BUNDLES  着色器包（.xgs 集合）；未提供时打一个空包
#   S2A_PIPELINE_SEED   管线预热种子；未提供则不带
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORKSPACE=$(cd "$ROOT/.." && pwd)

PRODUCT=$(cd "${1:?product dir}" && pwd)
TITLE=${2:?title id}
VERSION_CODE=${3:?version code}
VERSION_NAME=${4:?version name}
LABEL=${5:-}

S2A=$PRODUCT/game.s2a
[ -f "$S2A" ] || { echo "game.s2a 不存在：$S2A" >&2; exit 1; }

STAGE=$WORKSPACE/s2a-work/full-$TITLE
rm -rf "$STAGE"; mkdir -p "$STAGE"

# launch.json 从 game.s2a 的 index.json 反推，保证 bundle_id 与实际内容一致。
python3 - "$S2A" "$STAGE/launch.json" "$LABEL" <<'PY'
import hashlib, json, sys, zipfile
bundle, out, label = sys.argv[1], sys.argv[2], sys.argv[3]
with zipfile.ZipFile(bundle) as z:
    raw = z.read('index.json')
index = json.loads(raw)
doc = dict(schema=1, title_id=index['title_id'], bundle_id=hashlib.sha256(raw).hexdigest(),
           total_bytes=index['total_bytes'], name=label or index['title_id'], embedded=True)
json.dump(doc, open(out, 'w'))
print('launch.json:', json.dumps(doc)[:120])
PY
grep -q '"embedded": true' "$STAGE/launch.json" || { echo "launch.json 未标记 embedded" >&2; exit 1; }

# 着色器包是可选的：提供一个 .xgs 集合就随包发布；不提供就整条资产不放。
# ShaderBundles 对「资产不存在」是容错的（开发期热推路径），但对空包/非法条目会
# 直接抛 "着色器包为空"，所以这里宁可不放，也不要放一个占位包。
BUNDLES=${S2A_SHADER_BUNDLES:-}
WANT_BUNDLES=0
if [ -n "$BUNDLES" ] && [ -f "$BUNDLES" ]; then
    cp "$BUNDLES" "$STAGE/shader-bundles.zip"
    WANT_BUNDLES=1
    echo "shader-bundles: $(unzip -l "$BUNDLES" | tail -1)"
else
    echo "shader-bundles: 不随包（该标题只有运行时抓取）"
fi

cp "$ROOT/assets/shared-font.otf" "$STAGE/shared-font.otf"
SEED=${S2A_PIPELINE_SEED:-}
if [ -n "$SEED" ] && [ -f "$SEED" ]; then
    mkdir -p "$STAGE/pipeline-seed"
    cp "$SEED" "$STAGE/pipeline-seed/vk-pipeline-list.seed"
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
APPID=org.switch2android.g$TITLE

./gradlew --offline \
    -Ps2aAssets="$STAGE" \
    -Ps2aAppId="$APPID" \
    -Ps2aName="$NAME" \
    -Ps2aLabel="$LABEL" \
    -Ps2aVersionCode="$VERSION_CODE" \
    -Ps2aVersionName="$VERSION_NAME" \
    -Ps2aBuildDir="$ROOT/s2a-work/android-build-$TITLE" \
    assembleRelease

APK=$ROOT/s2a-work/android-build-$TITLE/app/outputs/apk/release/app-release.apk
for entry in launch.json game.s2a shared-font.otf; do
    unzip -l "$APK" | grep -q "assets/$entry" || { echo "打包结果缺 assets/$entry" >&2; exit 1; }
done
if [ "$WANT_BUNDLES" = 1 ]; then
    unzip -l "$APK" | grep -q "assets/shader-bundles.zip" || { echo "打包结果缺 assets/shader-bundles.zip" >&2; exit 1; }
fi
unzip -v "$APK" | awk '$8=="assets/game.s2a"{print "game.s2a method:", ($2=="Stored"?"Stored (ok)":"COMPRESSED ("$2")")}'

OUT=$WORKSPACE/dist/$TITLE-$VERSION_NAME-full.apk
cp "$APK" "$OUT"
echo "==> $OUT ($(du -h "$OUT" | cut -f1))"
