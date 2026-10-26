#!/usr/bin/env bash
# Builds shared-font.otf -- the face this host hands the title for every
# nn::pl shared font type except 5 (the Nintendo extension, which lives in
# native/nintendo_ext_font.h).
#
# Why this exists
# ---------------
# The console ships a full CJK face, so a Nintendo title's Chinese text is
# guaranteed to have a glyph for every character it writes. A port has to
# substitute something, and the substitution is not cosmetic: the title
# rasterises glyphs out of whatever face it is given, and a character the face
# lacks is drawn as the SDK's own notdef -- which in this title's engine is
# empty, so the character simply vanishes while its advance is kept. The result
# reads as gaps in the middle of sentences ("嗨" dropping out of "嗨！嗨！",
# "…" dropping out of "最近，大脑有点……？") rather than as boxes.
#
# The earlier face here was a GB2312-only subset (6763 hanzi, no U+2000..U+206F
# punctuation). Both halves of that were wrong: General Punctuation is where
# "…" "—" "''" "" live, and localised dialogue uses GBK-only hanzi such as 嗨,
# 啰, 妳 as well. This script keeps every GB2312 character, adds the most
# common GBK-only characters by frequency, and adds the punctuation blocks.
#
# Budget
# ------
# The face is not free. The title's font engine allocates from the guest heap
# once it has the face, and past a point it gets nothing back: an 11.3 MB
# (full GBK) face made the title die with a null dereference at 0x38 right
# after nvnTextureBuilderSetStride, and a 5.1 MB one did the same. 2.2 MB --
# what this script produces -- boots on the tablet this was measured on. Keep
# `--layout-features=` and `--no-hinting`: they are worth ~2.4x, and GSUB/GPOS
# buy nothing for a font the engine only reads outlines and metrics out of.
#
# Usage
#   tools/make-shared-font.sh [source-font[#face]] [output]
#
#   Source font defaults to the one Android ships (Noto Sans CJK SC lives at
#   face 2 of NotoSansCJK-Regular.ttc). Pass a .ttf/.otf path for anything else.
#   The default output is assets/shared-font.otf.
#
# Getting it onto the device (the game directory is the durable location, and
# it survives an app restart with no shell access needed):
#   adb push assets/shared-font.otf /sdcard/shared-font.otf
#   adb shell run-as <pkg> sh -c \
#     'cat /sdcard/shared-font.otf > files/games/<program-id>/shared-font.otf'
#
# Requires: python3 with fontTools, and optionally wordfreq for the frequency
# ranking (without it the hanzi set is GB2312 only, which is what used to be
# shipped, so install it).
set -euo pipefail

ROOT=$(cd "$(dirname "$0")/.." && pwd)
SRC=${1:-/system/fonts/NotoSansCJK-Regular.ttc#2}
OUT=${2:-$ROOT/assets/shared-font.otf}

# The device font is only at that path on a device; fall back to a local copy so
# this runs on a build machine too.
case "$SRC" in
    /system/fonts/*)
        PULL=${S2A_FONT_CACHE:-/tmp/s2a-font-cache}
        mkdir -p "$PULL"
        if [ ! -f "$PULL/$(basename "${SRC%%#*}")" ]; then
            ADB=${ADB:-adb}
            echo "== pulling ${SRC%%#*} via adb"
            "$ADB" pull "${SRC%%#*}" "$PULL/" >/dev/null
        fi
        SRC="$PULL/$(basename "${SRC%%#*}")${SRC#"${SRC%%#*}"}"
        ;;
esac
FACE=0
case "$SRC" in
    *#*) FACE=${SRC##*#}; SRC=${SRC%%#*} ;;
esac
[ -f "$SRC" ] || { echo "source font not found: $SRC" >&2; exit 1; }

WORK=$(mktemp -d)
trap 'rm -rf "$WORK"' EXIT

# The character set, in one place so it can be reviewed: every GB2312 hanzi,
# every hanzi wordfreq ranks among the most common, and the punctuation and
# symbol blocks a UI actually reaches for.
python3 - "$WORK/unicodes.txt" <<'PY'
import sys

hans = set()
# GB2312 level 1 + level 2: the 6763 characters a simplified-Chinese UI is
# written in. Anything here being absent is a bug, so it is the floor.
for hi in range(0xB0, 0xF8):
    for lo in range(0xA1, 0xFF):
        try:
            c = bytes([hi, lo]).decode("gb2312")
        except UnicodeDecodeError:
            continue
        if len(c) == 1:
            hans.add(ord(c))

try:
    from wordfreq import top_n_list
except ImportError:
    print("warning: wordfreq is not installed; the face will be GB2312 only and",
          "characters like 嗨 / 啰 will vanish from dialogue", file=sys.stderr)
    top_n_list = None
if top_n_list:
    # Single characters only, in frequency order, so the tail can be cut if the
    # budget ever has to shrink again.
    for word in top_n_list("zh", 400000):
        if len(word) == 1 and "\u4e00" <= word <= "\u9fff":
            hans.add(ord(word))

blocks = [
    # Basic Latin is not optional: without U+0020..U+007E the title's font
    # engine dies with a null dereference at 0x38 while it is still bringing the
    # face up, right after nvnTextureBuilderSetStride -- it is the space and the
    # digits it looks up first. This cost an afternoon; do not drop it because a
    # Chinese face "obviously" does not need ASCII.
    (0x0020, 0x007E),  # Basic Latin
    (0x00A0, 0x00FF),  # Latin-1 supplement
    (0x0100, 0x017F),  # Latin extended A
    (0x2000, 0x206F),  # general punctuation: … — ‘’ “” ‰ † ‡
    (0x2070, 0x209F),  # super/subscripts
    (0x20A0, 0x20BF),  # currencies
    (0x2100, 0x214F),  # letterlike
    (0x2150, 0x218F),  # number forms
    (0x2190, 0x21FF),  # arrows
    (0x2200, 0x22FF),  # maths operators
    (0x2300, 0x23FF),  # misc technical
    (0x2460, 0x24FF),  # enclosed alphanumerics
    (0x25A0, 0x25FF),  # geometric shapes
    (0x2600, 0x26FF),  # misc symbols
    (0x2700, 0x27BF),  # dingbats
    (0x3000, 0x303F),  # CJK punctuation
    (0x3040, 0x30FF),  # kana, for the odd 「・」
    (0xFE10, 0xFE1F),  # vertical forms
    (0xFE30, 0xFE4F),  # CJK compatibility forms
    (0xFF00, 0xFFEF),  # fullwidth forms
    (0xFF61, 0xFF9F),  # halfwidth katakana
]
cp = set(hans)
for lo, hi in blocks:
    cp.update(range(lo, hi + 1))

with open(sys.argv[1], "w") as fh:
    fh.write(",".join("U+%04X" % c for c in sorted(cp)))
print("characters requested: %d (%d hanzi)" % (len(cp), len([c for c in cp if 0x4E00 <= c <= 0x9FFF])))
PY

echo "== subsetting $SRC (face $FACE)"
mkdir -p "$(dirname "$OUT")"
python3 -m fontTools.subset "$SRC" \
    --font-number="$FACE" \
    --unicodes-file="$WORK/unicodes.txt" \
    --output-file="$OUT" \
    --layout-features= \
    --name-IDs='*' \
    --no-hinting \
    --notdef-outline \
    --recalc-bounds

SIZE=$(wc -c <"$OUT" | tr -d ' ')
echo "== $OUT ($SIZE bytes)"
# The title's own font engine came back empty-handed at 11.3 MB and at 5.1 MB on
# the tablet this was measured on; 2.2 MB is the shape that boots. Failing loudly
# here is better than a build that dies on device with a null dereference.
if [ "$SIZE" -gt 4500000 ]; then
    echo "!! over 4.5 MB: the title's font engine has been observed to fail above" >&2
    echo "   5 MB (null dereference at 0x38 after nvnTextureBuilderSetStride)." >&2
    echo "   Cut the least frequent hanzi from the frequency list before shipping." >&2
fi

python3 - "$OUT" <<'PY'
import sys
from fontTools.ttLib import TTFont
f = TTFont(sys.argv[1], lazy=True)
cm = f.getBestCmap()
probe = "…—‘’“”・嗨啰妳、。，？！有最近的你继续请我的"
missing = [c for c in probe if ord(c) not in cm]
print("== cmap %d, glyphs %d, tables %s" % (len(cm), f["maxp"].numGlyphs, ",".join(sorted(f.keys()))))
print("== probe missing: %s" % ("".join(missing) if missing else "(none)"))
sys.exit(1 if missing else 0)
PY
echo "== ok"
