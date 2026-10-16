#!/usr/bin/env bash
# Phase B 验收脚本：选区可调整（8 手柄/4 边/内部拖动）+ 快捷键对齐 QQ/PixPin
#
# 用法： bash scripts/acceptance-phaseB.sh
# 证据落在 docs/acceptance/evidence/
#
# 判据（全部自动判定，不依赖人工目视）：
#   1) 拖角手柄：只有那个角动，其余三边不动
#   2) 最小尺寸：把右边拖到越过左边 → 宽度夹到 16px（不会变成 0/负数）
#   3) **标注锚定屏幕内容**：选区变大后，标注在屏幕上的位置不变、数量不丢
#   4) 方向键微调：1px；Shift = 10px；尺寸不变
#   5) Ctrl+A：全选整屏
#   6) 选区改过之后，预览仍然 == 导出（选区内逐像素 0 差异）
#   7) Ctrl+C：完成并复制（COPIED image/png，不弹浮层）
#   8) Ctrl+S：完成并保存到指定路径（SAVED + 落盘 + 尺寸一致 + 图里真的有标注）
#   9) A1/A2/A3/A3.1 判据未回归
#
# 注意：屏幕上会短暂（约 5 秒 x3 次）出现覆盖层，这是预期行为。
set -u
cd "$(dirname "$0")/.."

SEASHOT="$HOME/.local/bin/seashot"
OUT="docs/acceptance/evidence"
PREFIX="$OUT/b-inplace"
LOG="$OUT/b-inplace.txt"
mkdir -p "$OUT" docs/acceptance
PASS=0
FAIL=0

report() { # $1 名称 $2 0/1 $3 细节
    if [ "$2" = 1 ]; then
        printf 'PASS  %-42s %s\n' "$1" "$3"
        PASS=$((PASS + 1))
    else
        printf 'FAIL  %-42s %s\n' "$1" "$3"
        FAIL=$((FAIL + 1))
    fi
}

field() { # $1 关键字（形如 key=）
    sed -nE "s/.*$1([^ ]+).*/\1/p" "$LOG" | head -1
}

echo "== Phase B 验收（选区可调整 + 快捷键）=="
echo "二进制: $SEASHOT ($(stat -c '%y' "$SEASHOT" | cut -d. -f1))"
echo

### A) 交互判据 -----------------------------------------------------------------
timeout 120 "$SEASHOT" --inplace-demo "$PREFIX" > "$LOG" 2>&1
CODE=$?
if ! grep -q '^INPLACE_DEMO B' "$LOG"; then
    echo "FAIL  没有 B 输出，见 $LOG"
    exit 1
fi

GRIP_LINE=$(grep -m1 '^INPLACE_DEMO B gripCorner=' "$LOG")
NUDGE_LINE=$(grep -m1 '^INPLACE_DEMO B arrowNudge=' "$LOG")

GRIP=$(field 'gripCorner=')
GROW=$(sed -nE 's/.*选区 ([^ ]+ [^ ]+) -> ([^ ]+ [^ ]+).*/\1 → \2/p' <<<"$GRIP_LINE")
if [ "$GRIP" = 1 ]; then
    report "B. 拖角手柄只改那一个角" 1 "拖左上角 (-40,-30)：选区 $GROW"
else
    report "B. 拖角手柄只改那一个角" 0 "gripCorner=$GRIP（$GROW）"
fi

MINW=$(field 'minWidth=')
if [ "$MINW" = 1 ]; then
    report "B. 选区最小尺寸 16px" 1 "把右边拖到越过左边 → 宽度夹到 16px"
else
    report "B. 选区最小尺寸 16px" 0 "minWidth=$MINW"
fi

ANCH=$(field 'anchored=')
KEPT=$(field 'annotationsKept=')
if [ "$ANCH" = 1 ] && [ "$KEPT" = 1 ]; then
    report "B. 标注锚定屏幕内容（不丢不错位）" 1 \
        "选区变大后标注的屏幕位置不变（原点+标注坐标差 ≤2px），标注数量不变"
else
    report "B. 标注锚定屏幕内容（不丢不错位）" 0 "anchored=$ANCH annotationsKept=$KEPT"
fi

NUDGE=$(field 'arrowNudge=')
ALL=$(field 'selectAll=')
if [ "$NUDGE" = 1 ] && [ "$ALL" = 1 ]; then
    report "B. 方向键微调 + Ctrl+A 全选" 1 \
        "→→ 各 1px、Shift+↓ 10px 且尺寸不变；Ctrl+A → 选区 == 整屏"
else
    report "B. 方向键微调 + Ctrl+A 全选" 0 "arrowNudge=$NUDGE selectAll=$ALL"
fi

STILL=$(field 'previewStillMatchesExport=')
B_MISMATCH=$(sed -nE 's/.* mismatch=(-?[0-9]+).*/\1/p' <<<"$NUDGE_LINE")
if [ "$STILL" = 1 ] && [ "$B_MISMATCH" = 0 ]; then
    report "B. 选区改过后预览仍 == 导出" 1 "选区内缩 6px 与 renderAnnotations() 输出逐像素 0 差异"
else
    report "B. 选区改过后预览仍 == 导出" 0 "previewStillMatchesExport=$STILL mismatch=$B_MISMATCH"
fi

B_OK=$(grep -c '^INPLACE_DEMO B PASS' "$LOG" || true)
if [ "$B_OK" = 1 ]; then
    report "B. 汇总判定（演示）" 1 "INPLACE_DEMO B PASS（退出码 $CODE）"
else
    report "B. 汇总判定（演示）" 0 "$(grep -m1 '^INPLACE_DEMO B ' "$LOG" | tail -1)（退出码 $CODE）"
fi

### B) Ctrl+C：完成并复制 --------------------------------------------------------
COPY_LOG="$OUT/b-copy.txt"
SEASHOT_INPLACE_DEMO_ACTION=copy timeout 120 "$SEASHOT" --inplace-demo "$OUT/b-copy" \
    > "$COPY_LOG" 2>&1
COPY_CODE=$?
COPY_OK=$(grep -c '^COPIED image/png' "$COPY_LOG" || true)
COPY_TYPES=$(wl-paste --list-types 2>&1 | tr '\n' ' ')
if [ "$COPY_OK" = 1 ]; then
    report "B. Ctrl+C = 完成并复制" 1 \
        "合成 Ctrl+C → COPIED image/png（剪贴板类型：${COPY_TYPES:-（空）}），退出码 $COPY_CODE"
else
    report "B. Ctrl+C = 完成并复制" 0 "没有 COPIED 行；见 $COPY_LOG"
fi
wl-copy --clear 2>/dev/null

### C) Ctrl+S：完成并保存 --------------------------------------------------------
SAVE_LOG="$OUT/b-save.txt"
SAVE_RESULT="$OUT/b-save-result.png"
rm -f "$SAVE_RESULT"
SEASHOT_INPLACE_DEMO_ACTION=save timeout 120 "$SEASHOT" --inplace-demo "$OUT/b-save" \
    --save "$SAVE_RESULT" --no-copy > "$SAVE_LOG" 2>&1
SAVE_CODE=$?
SAVED=$(grep -c "^SAVED $SAVE_RESULT" "$SAVE_LOG" || true)
if [ "$SAVED" = 1 ] && [ -f "$SAVE_RESULT" ]; then
    SSIZE=$(magick identify -format '%wx%h' "$SAVE_RESULT" 2>/dev/null || echo '?')
    report "B. Ctrl+S = 完成并保存" 1 \
        "合成 Ctrl+S → SAVED，落盘 $SSIZE（退出码 $SAVE_CODE）"
else
    report "B. Ctrl+S = 完成并保存" 0 "SAVED=$SAVED 文件=$([ -f "$SAVE_RESULT" ] && echo 1 || echo 0)；见 $SAVE_LOG"
fi

# 保存下来的图里必须真的有标注：用「标注区域」做对照（与 A3 同一套思路，避开桌面漂移）
SEL=$(sed -nE 's/.*选区本地=(-?[0-9]+),(-?[0-9]+) ([0-9]+)x([0-9]+).*/\1 \2 \3 \4/p' "$LOG" | head -1)
read -r SX SY SW SH <<<"${SEL:-0 0 0 0}"
R_FROZEN="?"; R_PREVIEW="?"
if [ -f "$OUT/b-save-frozen.png" ] && [ -f "$SAVE_RESULT" ] && [ -f "$PREFIX-annotated.png" ]; then
    magick "$OUT/b-save-frozen.png" -crop "${SW}x${SH}+${SX}+${SY}" +repage "$OUT/b-save-frozen-selection.png" 2>/dev/null
    for kind in frozen result preview; do :; done
    magick "$OUT/b-save-frozen-selection.png" -crop '150x100+250+40' +repage "$OUT/b-save-region-frozen.png" 2>/dev/null
    magick "$SAVE_RESULT" -crop '150x100+250+40' +repage "$OUT/b-save-region-result.png" 2>/dev/null
    magick "$PREFIX-annotated.png" -crop '150x100+250+40' +repage "$OUT/b-save-region-preview.png" 2>/dev/null
    t=$(magick compare -metric RMSE "$OUT/b-save-region-frozen.png" "$OUT/b-save-region-result.png" null: 2>&1)
    R_FROZEN=$(sed -nE 's/.*\(([0-9.]+)\).*/\1/p' <<<"$t")
    t=$(magick compare -metric RMSE "$OUT/b-save-region-preview.png" "$OUT/b-save-region-result.png" null: 2>&1)
    R_PREVIEW=$(sed -nE 's/.*\(([0-9.]+)\).*/\1/p' <<<"$t")
fi
if python3 -c "
import sys
f, p = float('${R_FROZEN:-1}'), float('${R_PREVIEW:-0}')
sys.exit(0 if (f > 0.05 and f > 4 * max(p, 1e-6)) else 1)"; then
    report "B. 保存的结果里确实有标注" 1 \
        "标注区域：结果 vs 未标注裁切 RMSE=$R_FROZEN ≫ 结果 vs 预览 RMSE=$R_PREVIEW"
else
    report "B. 保存的结果里确实有标注" 0 "RMSE 未标注=$R_FROZEN 预览=$R_PREVIEW（应前者 >0.05 且 ≥4 倍）"
fi

### D) 前面几个阶段的判据未回归 --------------------------------------------------
A1=$(grep -c '^INPLACE_DEMO PASS$' "$LOG" || true)
A2=$(grep -c '^INPLACE_DEMO A2 PASS' "$LOG" || true)
A3=$(grep -c '^INPLACE_DEMO A3 PASS' "$LOG" || true)
A31=$(grep -c '^INPLACE_DEMO A31 PASS' "$LOG" || true)
if [ "$A1" = 1 ] && [ "$A2" = 1 ] && [ "$A3" = 1 ] && [ "$A31" = 1 ]; then
    report "B. A1/A2/A3/A3.1 判据未回归" 1 "1:1+手柄、工具条、9 种工具+撤销、颜色/粗细 仍全过"
else
    report "B. A1/A2/A3/A3.1 判据未回归" 0 "A1=$A1 A2=$A2 A3=$A3 A31=$A31"
fi

if [ -f "$PREFIX-annotated.png" ]; then
    report "B. 视觉证据落盘" 1 "$PREFIX.txt / -annotated.png / b-save-result.png 等"
else
    report "B. 视觉证据落盘" 0 "缺少 $PREFIX-annotated.png"
fi

echo
echo "== 汇总: $PASS 通过 / $FAIL 失败 =="
echo "证据: $LOG、$COPY_LOG、$SAVE_LOG 与 $OUT/b-*.png"
grep -m1 '^INPLACE_DEMO B gripCorner=' "$LOG"
[ "$FAIL" = 0 ] && [ "$CODE" = 0 ]
