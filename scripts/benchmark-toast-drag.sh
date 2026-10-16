#!/usr/bin/env bash
# ISSUE-001 回归基准：结果浮层拖动延迟
#
# 做法：让 SeaShot 自己把浮层按 250px/s 匀速拖动，同时用**子进程**抓帧观测
# （已实测：同一进程的 ScreenShot2 请求拍不到自己的 layer-shell 表面，见 ISSUE-007），
# 用帧时间戳反查"当时的目标位置"，算出「显示位置落后目标多少像素/毫秒」。
#
# 判据：lagPxMax ≤ 8px（在 250px/s 下 ≈ 32ms，即两帧以内）
#
# 用法： bash scripts/benchmark-toast-drag.sh [毫秒]
set -u
cd "$(dirname "$0")/.."

SEASHOT="$HOME/.local/bin/seashot"
OUT="docs/acceptance/evidence"
DURATION="${1:-3000}"
mkdir -p "$OUT"

pkill -x seashot 2>/dev/null
sleep 0.5
echo "== 拖动延迟基准（拖动 ${DURATION}ms @ 250px/s）=="
timeout 120 "$SEASHOT" --toast-drag-bench "$DURATION" --rect 300,200,400,300 \
    > "$OUT/issue001-drag-bench.txt" 2>&1

SAMPLES=$(grep -c '^DRAGBENCH_SAMPLE' "$OUT/issue001-drag-bench.txt" || true)
SUMMARY=$(grep '^DRAGBENCH samples=' "$OUT/issue001-drag-bench.txt" | tail -1)
LAG_AVG=$(echo "$SUMMARY" | sed -nE 's/.*lagPxAvg=(-?[0-9.]+).*/\1/p')
LAG_MAX=$(echo "$SUMMARY" | sed -nE 's/.*lagPxMax=([0-9.-]+).*/\1/p')
LAG_MS=$(echo "$SUMMARY" | sed -nE 's/.*lagMsAvg=([0-9.]+).*/\1/p')
RELEASE=$(grep '^DRAGBENCH_RELEASE' "$OUT/issue001-drag-bench.txt" | tail -1)
RELEASE_OK=$(echo "$RELEASE" | sed -nE 's/.*ok=([01]).*/\1/p')
echo "落点: $RELEASE"

echo "样本数: $SAMPLES"
echo "$SUMMARY"

if [ -z "${LAG_MAX:-}" ]; then
    echo "FAIL  没能拿到有效观测（见 $OUT/issue001-drag-bench.txt）"
    pkill -x seashot 2>/dev/null
    exit 1
fi

# 允许 8px（两帧）；用整数比较避免 shell 小数问题
RESULT=0
if [ "${LAG_MAX%.*}" -gt 8 ] || [ "${LAG_MAX%.*}" -lt -8 ]; then
    echo "FAIL  拖动仍然滞后：lagPxAvg=$LAG_AVG lagPxMax=$LAG_MAX（阈值 8px）"
    RESULT=1
else
    echo "PASS  拖动跟随：lagPxAvg=$LAG_AVG lagPxMax=$LAG_MAX lagMsAvg=$LAG_MS（阈值 8px ≈ 32ms）"
fi
# 松手落点：必须停在拖到的位置（曾经因为没同步 pending 位置而瞬移回原位）
if [ "${RELEASE_OK:-0}" != "1" ]; then
    echo "FAIL  松手落点不对（$RELEASE）—— 检查 mouseMove 是否同步了 pending 位置"
    RESULT=1
else
    echo "PASS  松手落点正确：$RELEASE"
fi
echo "证据: $OUT/issue001-drag-bench.txt"
pkill -x seashot 2>/dev/null
exit "$RESULT"
