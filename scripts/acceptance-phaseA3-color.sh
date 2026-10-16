#!/usr/bin/env bash
# Phase A3.1 验收脚本：就地编辑的**颜色 / 粗细**（自绘弹出面板）
#
# 用法： bash scripts/acceptance-phaseA3-color.sh
# 证据落在 docs/acceptance/evidence/
#
# 判据（全部自动判定，不依赖人工目视）：
#   1) 工具条上多了「颜色」「粗细」两格（在工具条内、互不重叠）
#   2) 颜色面板：点色块 → 当前颜色变，且新画的标注真的用新颜色
#   3) 面板不压选区（标注区不被自己的 UI 盖住）
#   4) 粗细面板：拖滑杆 → 粗细变（与位置换算一致），且新画的标注真的用新粗细
#   5) 面板内的空白点击不产生标注；Esc 只关面板、不退出标注；面板外点击只关面板
#   6) A1/A2/A3 判据未回归
#
# 注意：屏幕上会短暂（约 4 秒）出现覆盖层，这是预期行为。
set -u
cd "$(dirname "$0")/.."

SEASHOT="$HOME/.local/bin/seashot"
OUT="docs/acceptance/evidence"
PREFIX="$OUT/a31-inplace"
LOG="$OUT/a31-inplace.txt"
mkdir -p "$OUT" docs/acceptance
PASS=0
FAIL=0

report() { # $1 名称 $2 0/1 $3 细节
    if [ "$2" = 1 ]; then
        printf 'PASS  %-40s %s\n' "$1" "$3"
        PASS=$((PASS + 1))
    else
        printf 'FAIL  %-40s %s\n' "$1" "$3"
        FAIL=$((FAIL + 1))
    fi
}

field() { # $1 关键字（形如 key=）
    sed -nE "s/.*$1([^ ]+).*/\1/p" "$LOG" | head -1
}

echo "== Phase A3.1 验收（颜色 / 粗细）=="
echo "二进制: $SEASHOT ($(stat -c '%y' "$SEASHOT" | cut -d. -f1))"
echo

timeout 90 "$SEASHOT" --inplace-demo "$PREFIX" > "$LOG" 2>&1
CODE=$?

if ! grep -q '^INPLACE_DEMO A31' "$LOG"; then
    echo "FAIL  没有 A31 输出，见 $LOG"
    exit 1
fi

COLOR_LINE=$(grep -m1 '^INPLACE_DEMO A31 cells=' "$LOG")
WIDTH_LINE=$(grep -m1 '^INPLACE_DEMO A31 widthPanel=' "$LOG")
QUIET_LINE=$(grep -m1 '^INPLACE_DEMO A31 panelClickQuiet=' "$LOG")

CELLS=$(field 'cells=')
if [ "$CELLS" = 1 ]; then
    report "A3.1 工具条多了颜色/粗细两格" 1 \
        "两格都在工具条内、互不重叠（$(sed -nE 's/.*工具条=([^ ]+ [^ ]+).*/工具条 \1/p' "$LOG" | head -1)）"
else
    report "A3.1 工具条多了颜色/粗细两格" 0 "cells=$CELLS"
fi

PANEL=$(field 'colorPanel=')
OUTSIDE=$(field 'outsideSelection=')
PICK=$(field 'swatchPick=')
GOT_COLOR=$(sed -nE 's/.* color=(#[0-9a-f]+).*/\1/p' <<<"$COLOR_LINE")
DRAWN_COLOR=$(sed -nE 's/.* drawnColor=(#[0-9a-f]+).*/\1/p' <<<"$COLOR_LINE")
if [ "$PANEL" = 1 ] && [ "$OUTSIDE" = 1 ] && [ "$PICK" = 1 ] && [ "$GOT_COLOR" = "$DRAWN_COLOR" ]; then
    report "A3.1 颜色面板：选色 → 标注跟着变色" 1 \
        "点色块 → 当前颜色 $GOT_COLOR，新画矩形的颜色也是 $DRAWN_COLOR（面板不压选区）"
else
    report "A3.1 颜色面板：选色 → 标注跟着变色" 0 \
        "panel=$PANEL outsideSelection=$OUTSIDE swatchPick=$PICK color=$GOT_COLOR drawn=$DRAWN_COLOR"
fi

WPANEL=$(field 'widthPanel=')
SLIDER=$(field 'slider=')
GOT_WIDTH=$(sed -nE 's/.* width=([0-9]+).*/\1/p' <<<"$WIDTH_LINE")
MEASURED=$(sed -nE 's/.*实测 ([0-9]+).*/\1/p' <<<"$WIDTH_LINE")
DRAWN_WIDTH=$(sed -nE 's/.*drawnWidth=([0-9]+).*/\1/p' <<<"$WIDTH_LINE")
if [ "$WPANEL" = 1 ] && [ "$SLIDER" = 1 ] && [ "$GOT_WIDTH" = "$MEASURED" ] \
    && [ "$GOT_WIDTH" = "$DRAWN_WIDTH" ]; then
    report "A3.1 粗细面板：拖滑杆 → 标注跟着变粗" 1 \
        "拖到滑杆 70% → 换算宽度 $GOT_WIDTH == 实测 $MEASURED，新画画笔的 width 也是 $DRAWN_WIDTH"
else
    report "A3.1 粗细面板：拖滑杆 → 标注跟着变粗" 0 \
        "widthPanel=$WPANEL slider=$SLIDER 换算=$GOT_WIDTH 实测=$MEASURED 标注=$DRAWN_WIDTH"
fi

QUIET=$(field 'panelClickQuiet=')
ESC=$(field 'escCloses=')
OUTQ=$(field 'outsideQuiet=')
if [ "$QUIET" = 1 ] && [ "$ESC" = 1 ] && [ "$OUTQ" = 1 ]; then
    report "A3.1 面板的点击语义" 1 \
        "面板内空白点击不产生标注；Esc 只关面板（仍在标注阶段）；面板外点击只关面板"
else
    report "A3.1 面板的点击语义" 0 "quiet=$QUIET esc=$ESC outside=$OUTQ"
fi

if grep -q '^INPLACE_DEMO A31 PASS' "$LOG"; then
    report "A3.1 汇总判定" 1 "INPLACE_DEMO A31 PASS（退出码 $CODE）"
else
    report "A3.1 汇总判定" 0 "$(grep -m1 '^INPLACE_DEMO A31' "$LOG" | tail -1)（退出码 $CODE）"
fi

A1=$(grep -c '^INPLACE_DEMO PASS$' "$LOG" || true)
A2=$(grep -c '^INPLACE_DEMO A2 PASS' "$LOG" || true)
A3=$(grep -c '^INPLACE_DEMO A3 PASS' "$LOG" || true)
if [ "$A1" = 1 ] && [ "$A2" = 1 ] && [ "$A3" = 1 ]; then
    report "A3.1 A1/A2/A3 判据未回归" 1 "1:1/手柄、工具条命中、9 种工具 + 撤销 + 预览==导出 仍全过"
else
    report "A3.1 A1/A2/A3 判据未回归" 0 "A1=$A1 A2=$A2 A3=$A3"
fi

# 面板的视觉证据 + 工具条那一带的放大图（人眼核对色板/滑杆/当前值）
BAR=$(sed -nE 's/^INPLACE_DEMO toolbar=(-?[0-9]+),(-?[0-9]+) ([0-9]+)x([0-9]+).*/\1 \2 \3 \4/p' "$LOG" | head -1)
read -r BX BY BW BH <<<"${BAR:-0 0 0 0}"
for shot in panel-color panel-width; do
    src="$PREFIX-$shot.png"
    [ -f "$src" ] || continue
    magick "$src" -crop "$((BW + 40))x$((BH + 150))+$((BX - 20))+$((BY - 10))" +repage \
        -resize 150% "$PREFIX-$shot-zoom.png" 2>/dev/null || true
done
if [ -f "$PREFIX-panel-color.png" ] && [ -f "$PREFIX-panel-width.png" ]; then
    report "A3.1 视觉证据落盘" 1 \
        "$PREFIX-panel-color.png / -panel-width.png 及各自的 -zoom.png（工具条 + 面板放大）"
else
    report "A3.1 视觉证据落盘" 0 "缺少 panel 截图"
fi

echo
echo "== 汇总: $PASS 通过 / $FAIL 失败 =="
echo "证据: $LOG 与 $PREFIX-panel-*.png"
grep -m1 '^INPLACE_DEMO A31' "$LOG"
[ "$FAIL" = 0 ] && [ "$CODE" = 0 ]
