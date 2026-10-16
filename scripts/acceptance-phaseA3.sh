#!/usr/bin/env bash
# Phase A3 验收脚本：标注的绘制 / 命中 / 撤销，以及「完成」把标注带进结果
#
# 用法： bash scripts/acceptance-phaseA3.sh
# 证据落在 docs/acceptance/evidence/
#
# 判据（全部自动判定，不依赖人工目视）：
#   1) 6 种「拖一下」的形状工具都能画出来（矩形/椭圆/箭头/高亮/马赛克/模糊）
#   2) 画笔（多点折线）、序号（单击）、文字（自绘输入，合成按键）都能落成标注
#   3) Ctrl+Z 撤销 / Ctrl+Shift+Z 重做（计数 9→8→9）
#   4) 选择工具的命中 + 拖动移动（矩形上边中点，位移与鼠标一致）
#   5) Delete 删除选中的标注
#   6) 工具条上拖拽不产生标注（A2 的判定策略在 A3 仍然成立）
#   7) **预览 == 导出**：屏幕上选区内的像素逐像素等于 renderAnnotations() 的输出（0 差异）
#   8) 「完成」出口把标注真的带进结果：`ANNOTATED count=N` + 落盘图的尺寸与选区一致
#   9) A1/A2 判据未回归
#
# 注意：屏幕上会短暂（约 3~6 秒）出现覆盖层，这是预期行为。
set -u
cd "$(dirname "$0")/.."

SEASHOT="$HOME/.local/bin/seashot"
OUT="docs/acceptance/evidence"
PREFIX="$OUT/a3-inplace"
LOG="$OUT/a3-inplace.txt"
COMMIT_PREFIX="$OUT/a3-commit"
COMMIT_LOG="$OUT/a3-commit.txt"
RESULT="$COMMIT_PREFIX-result.png"
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

echo "== Phase A3 验收（标注绘制 / 命中 / 撤销）=="
echo "二进制: $SEASHOT ($(stat -c '%y' "$SEASHOT" | cut -d. -f1))"
echo

### A) 交互判据：跑一遍就地标注演示（默认在最后点「取消」退出） ----------------
timeout 90 "$SEASHOT" --inplace-demo "$PREFIX" > "$LOG" 2>&1
CODE=$?

if ! grep -q '^INPLACE_DEMO' "$LOG"; then
    echo "FAIL  无法取得 INPLACE_DEMO 输出，见 $LOG"
    exit 1
fi

SHAPES=$(sed -nE 's/^INPLACE_DEMO A3 shapes=([0-9]+)\/([0-9]+).*/\1\/\2/p' "$LOG")
DRAWN=$(sed -nE 's/.*drawn=//p' "$LOG" | head -1)
if [ "$SHAPES" = "6/6" ]; then
    report "A3. 六种形状工具都能画出来" 1 "逐个合成拖拽 → $DRAWN"
else
    report "A3. 六种形状工具都能画出来" 0 "命中 $SHAPES，drawn=$DRAWN"
fi

PEN=$(field 'pen=')
NUM=$(field 'number=')
TXT=$(field 'text=')
TXT_VAL=$(sed -nE 's/.*textValue=("[^"]*").*/\1/p' "$LOG" | head -1)
if [ "$PEN" = 1 ] && [ "$NUM" = 1 ] && [ "$TXT" = 1 ] && [ "$TXT_VAL" = '"Hi 3"' ]; then
    report "A3. 画笔 / 序号 / 文字" 1 \
        "画笔折线、序号徽标 1、文字 $TXT_VAL（合成按键输入后回车提交）"
else
    report "A3. 画笔 / 序号 / 文字" 0 "pen=$PEN number=$NUM text=$TXT textValue=$TXT_VAL"
fi

UNDO=$(field 'undo=')
UNDO_COUNTS=$(sed -nE 's/.*计数 ([0-9]+)->([0-9]+)->([0-9]+).*/\1→\2→\3/p' "$LOG" | head -1)
if [ "$UNDO" = 1 ]; then
    report "A3. Ctrl+Z 撤销 / Ctrl+Shift+Z 重做" 1 "标注计数 $UNDO_COUNTS"
else
    report "A3. Ctrl+Z 撤销 / Ctrl+Shift+Z 重做" 0 "undo=$UNDO（$UNDO_COUNTS）"
fi

MOVE=$(field 'move=')
MOVE_BOUNDS=$(sed -nE 's/.*bounds=([^ ]+ [^ ]+).*/\1/p' "$LOG" | head -1)
if [ "$MOVE" = 1 ]; then
    report "A3. 选择工具命中 + 拖动移动" 1 "拖动 (+45,+28) 后 bounds=$MOVE_BOUNDS（与鼠标位移一致）"
else
    report "A3. 选择工具命中 + 拖动移动" 0 "move=$MOVE bounds=$MOVE_BOUNDS"
fi

DEL=$(field 'delete=')
DEL_COUNTS=$(sed -nE 's/.*A3 delete=[0-9]+ 计数 ([0-9]+)->([0-9]+).*/\1→\2/p' "$LOG" | head -1)
if [ "$DEL" = 1 ]; then
    report "A3. Delete 删除选中标注" 1 "标注计数 $DEL_COUNTS"
else
    report "A3. Delete 删除选中标注" 0 "delete=$DEL（$DEL_COUNTS）"
fi

QUIET=$(field 'toolbarDragQuiet=')
if [ "$QUIET" = 1 ]; then
    report "A3. 工具条上拖拽不产生标注" 1 "在工具条矩形内拖过 80px，标注数不变"
else
    report "A3. 工具条上拖拽不产生标注" 0 "toolbarDragQuiet=$QUIET"
fi

MATCH=$(field 'previewMatchesExport=')
MISMATCH=$(field 'mismatch=')
if [ "$MATCH" = 1 ] && [ "$MISMATCH" = 0 ]; then
    report "A3. 预览 == 导出（逐像素）" 1 \
        "屏幕上选区内缩 6px 与 renderAnnotations() 输出逐像素比对：0 差异"
else
    report "A3. 预览 == 导出（逐像素）" 0 "previewMatchesExport=$MATCH mismatch=$MISMATCH"
fi

if grep -q '^INPLACE_DEMO A3 PASS' "$LOG"; then
    report "A3. 汇总判定（演示）" 1 "INPLACE_DEMO A3 PASS（退出码 $CODE）"
else
    report "A3. 汇总判定（演示）" 0 "$(grep -m1 '^INPLACE_DEMO A3' "$LOG")（退出码 $CODE）"
fi

### B) 端到端：合成 Enter「完成」→ 标注被带进结果并落盘 -------------------------
SEL=$(sed -nE 's/.*选区本地=(-?[0-9]+),(-?[0-9]+) ([0-9]+)x([0-9]+).*/\1 \2 \3 \4/p' "$LOG" | head -1)
read -r SX SY SW SH <<<"$SEL"

rm -f "$RESULT"
SEASHOT_INPLACE_DEMO_COMMIT=1 timeout 90 "$SEASHOT" --inplace-demo "$COMMIT_PREFIX" \
    --save "$RESULT" --no-copy > "$COMMIT_LOG" 2>&1
COMMIT_CODE=$?

ANNOTATED=$(sed -nE 's/^ANNOTATED count=([0-9]+).*/\1/p' "$COMMIT_LOG" | tail -1)
CAPTURED=$(grep -c '^CAPTURE ok size=' "$COMMIT_LOG" || true)
if [ "${ANNOTATED:-0}" -gt 0 ] && [ "$CAPTURED" = 1 ] && [ -f "$RESULT" ]; then
    report "A3. 「完成」把标注带进结果" 1 \
        "合成 Enter → ANNOTATED count=$ANNOTATED → CAPTURE ok → SAVED（退出码 $COMMIT_CODE）"
else
    report "A3. 「完成」把标注带进结果" 0 \
        "ANNOTATED=${ANNOTATED:-（无）} CAPTURE_ok=$CAPTURED 落盘=$([ -f "$RESULT" ] && echo 1 || echo 0)；见 $COMMIT_LOG"
fi

RSIZE=$(magick identify -format '%wx%h' "$RESULT" 2>/dev/null || echo '?')
if [ "$RSIZE" = "${SW}x${SH}" ]; then
    report "A3. 结果尺寸 == 选区尺寸" 1 "落盘图 $RSIZE == 选区 ${SW}x${SH}（导出尺寸不变）"
else
    report "A3. 结果尺寸 == 选区尺寸" 0 "落盘图 $RSIZE ≠ 选区 ${SW}x${SH}"
fi

# 结果里必须真的有标注。直接比整张图会被「桌面内容在两次抓取之间变了」干扰，
# 所以用**区域对照**：挑一个确定被标注盖住的小区域（demo 里椭圆画在 250,40 150x100），
#   结果 应该像「预览（带标注）」而不是像「未标注的冻结裁切」。
# 实测（本机）：结果 vs 冻结裁切 ≈0.129，结果 vs 预览 ≈0.002 —— 判据留 4 倍差距，容错很大。
REGION="150x100+250+40"
FROZEN="$COMMIT_PREFIX-frozen.png"
SEL_CROP="$COMMIT_PREFIX-frozen-selection.png"
PREVIEW="$PREFIX-annotated.png"
R_FROZEN="?"; R_PREVIEW="?"
if [ -f "$FROZEN" ] && [ -n "${SX:-}" ] && [ -f "$RESULT" ] && [ -f "$PREVIEW" ]; then
    magick "$FROZEN" -crop "${SW}x${SH}+${SX}+${SY}" +repage "$SEL_CROP" 2>/dev/null
    magick "$SEL_CROP" -crop "$REGION" +repage "$COMMIT_PREFIX-region-frozen.png" 2>/dev/null
    magick "$RESULT" -crop "$REGION" +repage "$COMMIT_PREFIX-region-result.png" 2>/dev/null
    magick "$PREVIEW" -crop "$REGION" +repage "$COMMIT_PREFIX-region-preview.png" 2>/dev/null
    tmp=$(magick compare -metric RMSE "$COMMIT_PREFIX-region-frozen.png" "$COMMIT_PREFIX-region-result.png" null: 2>&1)
    R_FROZEN=$(sed -nE 's/.*\(([0-9.]+)\).*/\1/p' <<<"$tmp")
    tmp=$(magick compare -metric RMSE "$COMMIT_PREFIX-region-preview.png" "$COMMIT_PREFIX-region-result.png" null: 2>&1)
    R_PREVIEW=$(sed -nE 's/.*\(([0-9.]+)\).*/\1/p' <<<"$tmp")
fi
if python3 -c "
import sys
f, p = float('${R_FROZEN:-1}'), float('${R_PREVIEW:-0}')
sys.exit(0 if (f > 0.05 and f > 4 * max(p, 1e-6)) else 1)"; then
    report "A3. 结果里确实有标注（区域对照）" 1 \
        "标注区域 $REGION：结果 vs 未标注裁切 RMSE=$R_FROZEN ≫ 结果 vs 预览 RMSE=$R_PREVIEW"
else
    report "A3. 结果里确实有标注（区域对照）" 0 \
        "标注区域 $REGION：结果 vs 未标注裁切=$R_FROZEN，结果 vs 预览=$R_PREVIEW（应前者 >0.05 且 ≥4 倍）"
fi

### C) A1/A2 判据未回归 --------------------------------------------------------
A1_OK=$(grep -c '^INPLACE_DEMO PASS$' "$LOG" || true)
A2_OK=$(grep -c '^INPLACE_DEMO A2 PASS' "$LOG" || true)
if [ "$A1_OK" = 1 ] && [ "$A2_OK" = 1 ]; then
    report "A3. A1/A2 判据未回归" 1 "1:1/手柄/压暗 与 工具条命中/切换 仍然全过"
else
    report "A3. A1/A2 判据未回归" 0 "A1_PASS=$A1_OK A2_PASS=$A2_OK"
fi

if [ -f "$PREFIX-annotated.png" ] && [ -f "$PREFIX-annotated-overlay.png" ]; then
    report "A3. 视觉证据落盘" 1 \
        "$PREFIX-annotated.png（选区 + 标注，$(magick identify -format '%wx%h' "$PREFIX-annotated.png" 2>/dev/null)）与 $PREFIX-annotated-overlay.png（整屏）"
else
    report "A3. 视觉证据落盘" 0 "缺少 $PREFIX-annotated.png / -annotated-overlay.png"
fi

echo
echo "== 汇总: $PASS 通过 / $FAIL 失败 =="
echo "证据: $LOG、$COMMIT_LOG 与 $PREFIX-*.png / $COMMIT_PREFIX-*.png"
echo "$(grep -m1 '^INPLACE_DEMO A3 status' "$LOG")"
[ "$FAIL" = 0 ] && [ "$CODE" = 0 ]
