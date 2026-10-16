#!/usr/bin/env bash
# Phase A2 验收脚本：自绘紧凑图标工具条
#
# 用法： bash scripts/acceptance-phaseA2.sh
# 证据落在 docs/acceptance/evidence/
#
# 判据（全部自动判定，不依赖人工目视）：
#   1) 10 个工具按钮**全部可命中**（合成鼠标点击 → currentTool 变成对应工具）
#   2) 快捷键 1–9 与工具一一对应
#   3) 工具条**不遮挡选区**，且完全在表面内
#   4) 落在按钮之间的空隙上不切换工具（点击策略：工具条优先、不误触发标注）
#   5) hover 高亮命中正确的格子
#   6) 自动避让：选区贴底 → 工具条跑到上方；贴右边缘 → 往里收
#   7) 「取消」按钮真的接上了（点它退出标注阶段）
#   8) A1 的几何判据（1:1 / 逐像素 / 手柄）仍然全过
#
# 注意：屏幕上会短暂（约 2 秒）出现覆盖层，这是预期行为。
set -u
cd "$(dirname "$0")/.."

SEASHOT="$HOME/.local/bin/seashot"
OUT="docs/acceptance/evidence"
PREFIX="$OUT/a2-inplace"
LOG="$OUT/a2-inplace.txt"
mkdir -p "$OUT" docs/acceptance
PASS=0
FAIL=0

report() { # $1 名称 $2 0/1 $3 细节
    if [ "$2" = 1 ]; then
        printf 'PASS  %-38s %s\n' "$1" "$3"
        PASS=$((PASS + 1))
    else
        printf 'FAIL  %-38s %s\n' "$1" "$3"
        FAIL=$((FAIL + 1))
    fi
}

field() { # $1 关键字（形如 key=），从 INPLACE_DEMO 日志里取值
    sed -nE "s/.*$1([^ ]+).*/\1/p" "$LOG" | head -1
}

echo "== Phase A2 验收（自绘图标工具条）=="
echo "二进制: $SEASHOT ($(stat -c '%y' "$SEASHOT" | cut -d. -f1))"
echo

timeout 60 "$SEASHOT" --inplace-demo "$PREFIX" > "$LOG" 2>&1
CODE=$?

if ! grep -q '^INPLACE_DEMO' "$LOG"; then
    echo "FAIL  无法取得 INPLACE_DEMO 输出，见 $LOG"
    exit 1
fi

TOOLS_LINE=$(grep -m1 '^INPLACE_DEMO tools=' "$LOG")
TOOLBAR_LINE=$(grep -m1 '^INPLACE_DEMO toolbar=' "$LOG")
HITS_LINE=$(grep -m1 '^INPLACE_DEMO toolHits=' "$LOG")
KEYS_LINE=$(grep -m1 '^INPLACE_DEMO keyHits=' "$LOG")
STATE_LINE=$(grep -m1 '^INPLACE_DEMO barOutsideSelection=' "$LOG")
AVOID_LINE=$(grep -m1 '^INPLACE_DEMO barGoesAboveWhenNoRoom=' "$LOG")
DIAG_LINE=$(grep -m1 '^INPLACE_DEMO annotating=' "$LOG")

EXPECT_TOOLS="选择/矩形/椭圆/箭头/画笔/高亮/文字/序号/马赛克/模糊"
GOT_TOOLS=$(sed 's/^INPLACE_DEMO tools=//; s/ keys=.*//' <<<"$TOOLS_LINE")
GOT_KEYS=$(sed 's/.* keys=//; s/ .*//' <<<"$TOOLS_LINE")
GOT_ICONS=$(sed 's/.* keys=[^ ]*//' <<<"$TOOLS_LINE")

if [ "$GOT_TOOLS" = "$EXPECT_TOOLS" ] && [ "$GOT_KEYS" = "1/2/3/4/5/6/7/8/9/-" ]; then
    report "A2. 工具按钮与快捷键表" 1 \
        "10 个：$GOT_TOOLS（顺序与 QQ/PixPin 一致，模糊无数字键）；$GOT_ICONS"
else
    report "A2. 工具按钮与快捷键表" 0 "实际=$GOT_TOOLS keys=$GOT_KEYS"
fi

TOOL_HITS=$(sed -nE 's/^INPLACE_DEMO toolHits=([0-9]+)\/([0-9]+).*/\1\/\2/p' <<<"$HITS_LINE")
SWITCHED=$(sed -nE 's/.*switched=//p' <<<"$HITS_LINE")
if [ "$TOOL_HITS" = "10/10" ]; then
    report "A2. 10 个按钮全部可命中" 1 \
        "逐个合成点击 → 工具依次切到 $SWITCHED"
else
    report "A2. 10 个按钮全部可命中" 0 "命中 $TOOL_HITS，switched=$SWITCHED"
fi

KEY_HITS=$(sed -nE 's/^INPLACE_DEMO keyHits=([0-9]+)\/([0-9]+).*/\1\/\2/p' <<<"$KEYS_LINE")
if [ "$KEY_HITS" = "9/9" ]; then
    report "A2. 快捷键 1–9 切换工具" 1 "1→选择 … 9→马赛克，逐条实测 9/9"
else
    report "A2. 快捷键 1–9 切换工具" 0 "命中 $KEY_HITS"
fi

OUTSIDE=$(field 'barOutsideSelection=')
INSIDE=$(field 'barInsideSurface=')
if [ "$OUTSIDE" = 1 ] && [ "$INSIDE" = 1 ]; then
    report "A2. 工具条不遮挡选区且在表面内" 1 \
        "$(sed 's/^INPLACE_DEMO //; s/ toolButtons.*//' <<<"$TOOLBAR_LINE")"
else
    report "A2. 工具条不遮挡选区且在表面内" 0 "outside=$OUTSIDE inside=$INSIDE"
fi

GAP=$(field 'gapQuiet=')
HOVER=$(field 'hover=')
HOVERCELL=$(field 'hoverCell=')
if [ "$GAP" = 1 ] && [ "$HOVER" = 1 ] && [ "$HOVERCELL" = 3 ]; then
    report "A2. 空隙不误切 + hover 高亮" 1 \
        "按钮间 2px 空隙点击不切换；鼠标移到第 4 个按钮 → hoverCell=$HOVERCELL"
else
    report "A2. 空隙不误切 + hover 高亮" 0 \
        "gapQuiet=$GAP hover=$HOVER hoverCell=$HOVERCELL"
fi

ABOVE=$(field 'barGoesAboveWhenNoRoom=')
CLAMPED=$(field 'barClampedAtRightEdge=')
EDGE=$(sed -nE 's/.*edgeBar=([^ ]+ [^ ]+).*/\1/p' <<<"$AVOID_LINE")
if [ "$ABOVE" = 1 ] && [ "$CLAMPED" = 1 ]; then
    report "A2. 工具条自动避让" 1 \
        "选区贴底→上方；选区贴右边缘→收进表面内（$EDGE）"
else
    report "A2. 工具条自动避让" 0 "above=$ABOVE clamped=$CLAMPED"
fi

CANCEL=$(field 'cancelButton=')
if [ "$CANCEL" = 1 ]; then
    report "A2. 「取消」按钮接上现有链路" 1 "点它之后退出标注阶段（annotationCancelled）"
else
    report "A2. 「取消」按钮接上现有链路" 0 "cancelButton=$CANCEL"
fi

A1_SUMMARY=$(grep -m1 '^INPLACE_DEMO frozen=' "$LOG")
A1_MISMATCH=$(sed -nE 's/.*exactMismatch=(-?[0-9]+).*/\1/p' <<<"$A1_SUMMARY")
A1_HANDLES=$(sed -nE 's/.*handles=([0-9]+)\/8.*/\1/p' <<<"$A1_SUMMARY")
if grep -q '^INPLACE_DEMO PASS$' "$LOG" && [ "$A1_MISMATCH" = 0 ] && [ "$A1_HANDLES" = 8 ]; then
    report "A2. A1 判据未回归" 1 "1:1 逐像素零差异 + 8 手柄仍全过"
else
    report "A2. A1 判据未回归" 0 "$A1_SUMMARY"
fi

if grep -q '^INPLACE_DEMO A2 PASS' "$LOG" && [ "$CODE" = 0 ]; then
    report "A2. 汇总判定" 1 "INPLACE_DEMO A2 PASS（退出码 0）"
else
    report "A2. 汇总判定" 0 "$(grep -m1 '^INPLACE_DEMO A2' "$LOG")（退出码 $CODE）"
fi

if [ -f "$PREFIX-overlay.png" ]; then
    # 顺手把工具条裁出来放大 2 倍：人眼核对图标/高亮/尺寸文字时用（每次跑都重生成）
    read -r BX BY BW BH < <(sed -nE 's/^INPLACE_DEMO toolbar=(-?[0-9]+),(-?[0-9]+) ([0-9]+)x([0-9]+).*/\1 \2 \3 \4/p' <<<"$TOOLBAR_LINE")
    if [ -n "${BX:-}" ]; then
        magick "$PREFIX-overlay.png" -crop "$((BW + 16))x$((BH + 24))+$((BX - 8))+$((BY - 8))" \
            +repage -resize 200% "$PREFIX-toolbar-2x.png" 2>/dev/null || true
    fi
    report "A2. 视觉证据落盘" 1 \
        "$PREFIX-overlay.png ($(magick identify -format '%wx%h' "$PREFIX-overlay.png" 2>/dev/null || echo '?')) + $PREFIX-toolbar-2x.png（工具条 2 倍放大）"
else
    report "A2. 视觉证据落盘" 0 "缺少 $PREFIX-overlay.png"
fi

echo
echo "== 汇总: $PASS 通过 / $FAIL 失败 =="
echo "证据: $LOG 与 $PREFIX-*.png"
echo "$DIAG_LINE"
[ "$FAIL" = 0 ] && [ "$CODE" = 0 ]
