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

# 默认产物指向 s2a-out-110：那是已合入官方 1.1.0 更新包（UPD v65536）的 game.s2a。
# 别指回 s2a-out-diag —— 那是更新前的基础版内容，用它打出来的包会"看起来是新版本、
# 实际跑旧内容"。带 UPD 的目录靠 package.json 的 version=65536 区分。
PRODUCT=${1:-$WORKSPACE/s2a-out-110/0100620012d6e000-v65536}
VERSION_CODE=${2:-221}
VERSION_NAME=${3:-2.0.1}

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
# launch.json 的 bundle_id 必须等于 game.s2a 里 index.json 的 SHA-256，
# BundleInstaller 会当场比对，不一致就判包损坏。所以**只能**由本目录的
# package.json 生成；千万别沿用上一个发布包的 launch.json —— 换游戏版本
# （比如合入 UPD）后 bundle_id 一定变，沿用等于主动把包装坏。
if [ -f "$PRODUCT/launch.json" ]; then
    cp "$PRODUCT/launch.json" "$STAGE/launch.json"
    echo "launch.json: 取自 $PRODUCT/launch.json"
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
# 交叉校验：launch.json 的 bundle_id 必须能在 game.s2a 的 index.json 上复现。
python3 - "$S2A" "$STAGE/launch.json" <<'PY'
import hashlib, json, sys, zipfile
want = json.load(open(sys.argv[2]))['bundle_id']
with zipfile.ZipFile(sys.argv[1]) as z:
    got = hashlib.sha256(z.read('index.json')).hexdigest()
if want != got:
    raise SystemExit(f'bundle_id 不匹配：launch.json={want} game.s2a={got}')
print('bundle_id 校验通过:', want[:16] + '…')
PY
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
