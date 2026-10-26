#!/usr/bin/env bash
# 生成随 APK 发布的 d2r 着色器包资产：<workspace>/s2a-work/shader-bundles.zip
#
# 为什么需要它：d2r 引擎只在 <files>/games/<title>/shaders/vk 里找 <key>.xgs，
# game.s2a 里那份 .spv/.res.json 是离线导出器的参考件，运行时一个都不读。这些包
# 是 dmsh_vk.py 离线导出后攒起来的，不随资产走的话，空白设备（含卸载重装）一进
# compute dispatch 就会 "Dispatch has no genuine compute shader" 直接退出。
#
# 唯一来源是 <workspace>/s2a-work/shader-bundles-source，它必须是**在设备上跑通过**
# 的那一份快照。早期版本取 s2a-xgs/ ∪ s2a-work/xgs-fix/，结果 150 个包是旧版本，
# 覆盖上去之后人物直接丢失 —— 目录名看着对不代表内容对，别再混用。
#
# 用法: tools/pack-shader-bundles.sh [输出路径]
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
WORKSPACE=$(cd "$ROOT/.." && pwd)
OUT=${1:-$WORKSPACE/s2a-work/shader-bundles.zip}
SOURCE=${S2A_SHADER_SOURCE:-$WORKSPACE/s2a-work/shader-bundles-source}
EXPECTED=${S2A_EXPECTED_BUNDLES:-1965}

[ -d "$SOURCE" ] || { echo "缺少快照目录 $SOURCE" >&2; exit 1; }

mkdir -p "$(dirname "$OUT")"
python3 - "$SOURCE" "$OUT" "$EXPECTED" <<'PY'
import os, sys, zipfile

source, out, expected = sys.argv[1], sys.argv[2], int(sys.argv[3])
names = sorted(n for n in os.listdir(source) if n.endswith(".xgs"))
# 名字必须是内容哈希的十六进制形式，这条同时挡住目录里的杂物。
bad = [n for n in names if len(n) != 20 or not all(c in "0123456789abcdef" for c in n[:16])]
if bad:
    raise SystemExit("非法包名: %s" % bad[:5])
if len(names) != expected:
    raise SystemExit("快照里 %d 个包，期望 %d" % (len(names), expected))

with zipfile.ZipFile(out, "w", zipfile.ZIP_DEFLATED, compresslevel=6) as archive:
    for name in names:
        archive.write(os.path.join(source, name), name)

print("%s: %d 包 / %d 字节" % (out, len(names), os.path.getsize(out)))
PY
