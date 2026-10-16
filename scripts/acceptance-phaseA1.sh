#!/usr/bin/env bash
# Phase A1 验收脚本：就地编辑骨架（覆盖层进入 Annotating 阶段）
#
# 用法： bash scripts/acceptance-phaseA1.sh
# 证据落在 docs/acceptance/evidence/
#
# 它回答的是 Phase A1 的四条判据（都是自动判定的，不依赖人工目视）：
#   1) 选完**不打开新窗口** —— 可见顶层部件只有覆盖层自己
#   2) 冻结屏幕按**逻辑像素 1:1** 画上去 —— 冻结图尺寸 == 覆盖层表面尺寸
#   3) 选区内部**逐像素**等于冻结图（无缩放/无错位）—— 内缩 4px 避开边框与手柄
#   4) 选区外被压暗、选区边框 + 8 个手柄真的画出来了
#
# 注意：屏幕上会短暂（约 2 秒）出现覆盖层，这是预期行为。
set -u
cd "$(dirname "$0")/.."

SEASHOT="$HOME/.local/bin/seashot"
OUT="docs/acceptance/evidence"
PREFIX="$OUT/a1-inplace"
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

echo "== Phase A1 验收（就地编辑骨架）=="
echo "二进制: $SEASHOT ($(stat -c '%y' "$SEASHOT" | cut -d. -f1))"
echo

# 一次性 CLI：失败时 process 也要退干净，别把后面几段挂住
timeout 60 "$SEASHOT" --inplace-demo "$PREFIX" > "$OUT/a1-inplace.txt" 2>&1
CODE=$?
LOG="$OUT/a1-inplace.txt"
LINE=$(grep -m1 '^INPLACE_DEMO annotating=' "$LOG" || true)
SUMMARY=$(grep -m1 '^INPLACE_DEMO frozen=' "$LOG" || true)

if [ -n "$LINE" ] && grep -q 'annotating=1 stage=Annotating' <<<"$LINE"; then
    report "A1. 选完进入 Annotating 阶段" 1 "$(sed 's/^INPLACE_DEMO //' <<<"$LINE")"
else
    report "A1. 选完进入 Annotating 阶段" 0 "见 $LOG"
fi

TOPS=$(grep -m1 '^INPLACE_DEMO visibleTops=' "$LOG" | sed 's/^INPLACE_DEMO visibleTops=//')
if [ "$TOPS" = "seashot-overlay" ]; then
    report "A1. 不弹新窗口（可见顶层部件）" 1 "只有 seashot-overlay，无 EditorWindow"
else
    report "A1. 不弹新窗口（可见顶层部件）" 0 "可见顶层部件=「$TOPS」（期望只有 seashot-overlay）"
fi

if [ -n "$SUMMARY" ]; then
    FROZEN=$(sed -nE 's/.*frozen=([0-9]+x[0-9]+).*/\1/p' <<<"$SUMMARY")
    SURFACE=$(sed -nE 's/.*surface=([0-9]+x[0-9]+).*/\1/p' <<<"$SUMMARY")
    MISMATCH=$(sed -nE 's/.*exactMismatch=(-?[0-9]+).*/\1/p' <<<"$SUMMARY")
    MAXDIFF=$(sed -nE 's/.*maxChannelDiff=(-?[0-9]+).*/\1/p' <<<"$SUMMARY")
    HANDLES=$(sed -nE 's/.*handles=([0-9]+)\/8.*/\1/p' <<<"$SUMMARY")
    DIMOK=$(sed -nE 's/.*dimProbe=([0-9]+)\/([0-9]+).*/\1/p' <<<"$SUMMARY")
    DIMN=$(sed -nE 's/.*dimProbe=([0-9]+)\/([0-9]+).*/\2/p' <<<"$SUMMARY")

    if [ "$FROZEN" = "$SURFACE" ] && [ -n "$FROZEN" ]; then
        report "A1. 冻结屏幕 1:1（尺寸一致）" 1 "冻结图 $FROZEN == 覆盖层表面 $SURFACE"
    else
        report "A1. 冻结屏幕 1:1（尺寸一致）" 0 "冻结图=$FROZEN 表面=$SURFACE"
    fi

    if [ "${MISMATCH:-1}" = 0 ]; then
        report "A1. 选区内逐像素一致（无缩放误差）" 1 \
            "内缩 4px 逐像素比对：不一致 ${MISMATCH}px，最大通道差 ${MAXDIFF}"
    else
        report "A1. 选区内逐像素一致（无缩放误差）" 0 \
            "不一致 ${MISMATCH}px（应为 0），最大通道差 ${MAXDIFF}"
    fi

    if [ "${HANDLES:-0}" = 8 ] && [ "${DIMOK:-0}" = "${DIMN:-1}" ] && [ "${DIMN:-0}" -gt 0 ]; then
        report "A1. 选区边框/手柄 + 区外压暗" 1 \
            "8 个手柄命中 ${HANDLES}/8；区外压暗探针 ${DIMOK}/${DIMN} 符合 ×0.529"
    else
        report "A1. 选区边框/手柄 + 区外压暗" 0 \
            "手柄命中 ${HANDLES:-?}/8；区外压暗探针 ${DIMOK:-?}/${DIMN:-?}"
    fi
else
    report "A1. 冻结屏幕 1:1（尺寸一致）" 0 "缺少 $SUMMARY"
fi

if [ -f "$PREFIX-overlay.png" ] && [ -f "$PREFIX-frozen.png" ]; then
    report "A1. 视觉证据落盘" 1 \
        "$PREFIX-overlay.png ($(magick identify -format '%wx%h' "$PREFIX-overlay.png" 2>/dev/null || echo '?')) / -frozen.png"
else
    report "A1. 视觉证据落盘" 0 "缺少 $PREFIX-overlay.png / -frozen.png"
fi

echo
echo "== 汇总: $PASS 通过 / $FAIL 失败 =="
echo "证据: $LOG 与 $PREFIX-*.png"
[ "$FAIL" = 0 ] && [ "$CODE" = 0 ]
