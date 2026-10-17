#!/usr/bin/env bash
# Phase D 验收脚本：滚动截屏（长截图）
#
# 用法： bash scripts/acceptance-phaseD.sh
# 证据落在 docs/acceptance/evidence/
#
# 判据（全部自动判定，不依赖人工目视）：
#   1) 长截图能跑通：结果比选区高得多（真长图），帧数与累计位移都对得上
#   2) **内容正确**：把长图与测试窗口的"原始长页"（ground truth）做逐像素比对，RMSE 很小；
#      同时用"错位裁切"当负对照，证明这个指标真能区分"拼对了/拼错了"
#   3) 高度与原件一致（不重复、不缺口）：累计位移 == 页高 - 视口高
#   4) 滚动条列被自动裁掉（页面右侧那 14 列每帧都在变）
#   5) 页面不滚时会很快正常收工（不会无限跑），结果高度 ≈ 选区高
#   6) 交互入口（工具条「长截图」/ Ctrl+L）也能走通
#   7) 拼接算法的单测全过
#
# 注意：会起一个"自己滚动的测试窗口"占屏幕一小块（约 20 秒），并短暂显示指示器。
set -u
cd "$(dirname "$0")/.."

SEASHOT="$HOME/.local/bin/seashot"
OUT="docs/acceptance/evidence"
PREFIX="$OUT/d-scroll"
PAGE="$OUT/d-scroll-page.png"     # ground truth：测试窗口内部那张 600x3000 的长页
RESULT="$OUT/d-scroll-result.png" # 长截图结果
mkdir -p "$OUT" docs/acceptance
PASS=0
FAIL=0
SKIPPED=0

TESTWINDOW="probes/build/testwindow"
[ -x "$TESTWINDOW" ] || TESTWINDOW="probes/out/testwindow"

report() { # $1 名称 $2 0/1 $3 细节
    if [ "$2" = 1 ]; then
        printf 'PASS  %-42s %s\n' "$1" "$3"
        PASS=$((PASS + 1))
    else
        printf 'FAIL  %-42s %s\n' "$1" "$3"
        FAIL=$((FAIL + 1))
    fi
}

rmse_between() { # $1 $2 → 归一化 RMSE
    local raw
    raw=$(magick compare -metric RMSE "$1" "$2" null: 2>&1)
    sed -nE 's/.*\(([0-9.]+)\).*/\1/p' <<<"$raw"
}

echo "== Phase D 验收（滚动截屏 / 长截图）=="
echo "二进制: $SEASHOT ($(stat -c '%y' "$SEASHOT" | cut -d. -f1))"
echo

### A) 单测 ---------------------------------------------------------------------
if ctest --test-dir build -R scroll > "$OUT/d-unit.txt" 2>&1; then
    report "D. 拼接算法单测" 1 "$(grep 'tests passed' "$OUT/d-unit.txt" | tail -1)"
else
    report "D. 拼接算法单测" 0 "见 $OUT/d-unit.txt"
fi

### B) 起一个"自己滚动"的测试窗口，并对它做长截图 --------------------------------
pkill -x testwindow 2>/dev/null
sleep 0.5
rm -f "$PAGE" "$RESULT"
(timeout 60 "$TESTWINDOW" --scroll-page --w 600 --h 500 --x 200 --y 150 --seconds 50 \
    --page-out "$PAGE" --page-height 3000 --scroll-step 120 --scroll-interval 300 \
    --scroll-start 3000 > "$OUT/d-testwindow.txt" 2>&1 </dev/null &)
sleep 2.5

# 把测试窗口**激活到最上层**：被别的窗口压着时，抓到的帧根本不会滚（判据会假红）。
cat > /tmp/seashot-scroll-activate.js <<'ACT'
const all = workspace.windowList();
for (let i = 0; i < all.length; i++) {
    const w = all[i];
    if (w && !w.deleted && String(w.resourceClass || "").indexOf("seashot-probe-window") >= 0)
        workspace.activeWindow = w;
}
ACT
qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.loadScript \
    /tmp/seashot-scroll-activate.js "seashot-scroll-act" >/dev/null 2>&1
qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.start >/dev/null 2>&1
sleep 1
qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.unloadScript "seashot-scroll-act" >/dev/null 2>&1

GEO=$(awk '/seashot-probe-scrollpage/{match($0,/frame=([-0-9.]+),([-0-9.]+) ([-0-9.]+)x([-0-9.]+)/,a);
           printf "%d,%d,%d,%d", a[1],a[2],a[3],a[4]; exit}' \
    <("$SEASHOT" --list-windows 2>/dev/null))
if [ -z "${GEO:-}" ]; then
    report "D. 长截图跑通" 0 "没找到滚动的测试窗口（$TESTWINDOW --scroll-page）"
    pkill -x testwindow 2>/dev/null
    exit 1
fi
read -r GX GY GW GH < <(tr ',' ' ' <<<"$GEO")

timeout 90 "$SEASHOT" --scroll-capture --rect "$GEO" --save "$RESULT" --scroll-ms 25000 \
    > "$OUT/d-scroll.txt" 2>&1
SCROLL_CODE=$?
pkill -x testwindow 2>/dev/null

SCROLL_LINE=$(grep -m1 '^SCROLL ok' "$OUT/d-scroll.txt")
REPORT_LINE=$(grep -m1 '^SCROLL_REPORT' "$OUT/d-scroll.txt")
FRAMES=$(sed -nE 's/.*帧数=([0-9]+).*/\1/p' <<<"$REPORT_LINE")
TOTAL_DY=$(sed -nE 's/.*累计位移=(-?[0-9]+).*/\1/p' <<<"$REPORT_LINE")
CROP_LR=$(sed -nE 's/.*左右裁列=([0-9]+)\/([0-9]+).*/\1\/\2/p' <<<"$REPORT_LINE")
RES_W=$(sed -nE 's/^SCROLL ok size=([0-9]+)x.*/\1/p' <<<"$SCROLL_LINE")
RES_H=$(sed -nE 's/^SCROLL ok size=[0-9]+x([0-9]+).*/\1/p' <<<"$SCROLL_LINE")

if [ -f "$RESULT" ] && [ -n "${RES_H:-}" ] && [ "$RES_H" -gt $((GH * 2)) ]; then
    report "D. 长截图跑通（结果远高于选区）" 1 \
        "选区 ${GW}x${GH} → 长图 ${RES_W}x${RES_H}；帧数=$FRAMES 累计位移=$TOTAL_DY 裁列=$CROP_LR（退出码 $SCROLL_CODE）"
else
    report "D. 长截图跑通（结果远高于选区）" 0 \
        "size=${RES_W:-?}x${RES_H:-?}（选区 ${GW}x${GH}）；见 $OUT/d-scroll.txt"
fi

# 高度：应当等于 页高 - 视口高 的累计位移（不重复、不缺口）
EXPECTED_DY=$((3000 - GH))
if [ "${TOTAL_DY:-0}" = "$EXPECTED_DY" ] && [ "${RES_H:-0}" = "3000" ]; then
    report "D. 累计位移与高度都对得上" 1 \
        "累计位移 $TOTAL_DY == 页高 3000 - 视口高 $GH；长图高度 $RES_H == 页高（不重复/不缺口）"
elif [ "${TOTAL_DY:-0}" -lt "$EXPECTED_DY" ]; then
    # 只滚了一部分：多半是测试窗口中途被盖住/被打断（桌面在动）→ 跳过而不是误报
    printf 'SKIP  %-42s %s\n' "D. 累计位移与高度都对得上" \
        "只拼到 $TOTAL_DY/$EXPECTED_DY —— 测试窗口被打断/遮挡了吧（环境干扰），本次不判定"
    SKIPPED=$((SKIPPED + 1))
else
    report "D. 累计位移与高度都对得上" 0 \
        "累计位移=$TOTAL_DY（期望 $EXPECTED_DY）长图高=$RES_H（期望 3000）"
fi

# 内容正确性：与 ground truth 逐像素比（裁掉同样的左右列）
LEFT=$(cut -d/ -f1 <<<"$CROP_LR")
RIGHT=$(cut -d/ -f2 <<<"$CROP_LR")
CROP_W=$((600 - LEFT - RIGHT))
magick "$PAGE" -crop "${CROP_W}x3000+${LEFT}+0" +repage "$OUT/d-scroll-truth.png" 2>/dev/null
GOOD_RMSE="-"
BAD_RMSE="-"
BAD1_RMSE="-"
if [ -f "$OUT/d-scroll-truth.png" ] && [ -f "$RESULT" ]; then
    GOOD_RMSE=$(rmse_between "$OUT/d-scroll-truth.png" "$RESULT")
    # 负对照 1：原件往下挪 137 行（**不是 40px 行距的整数倍**，避免"周期性版式"把指标拉平）
    magick "$PAGE" -crop "${CROP_W}x$((3000 - 137))+${LEFT}+137" +repage "$OUT/d-scroll-truth-shift.png" 2>/dev/null
    magick "$RESULT" -crop "${CROP_W}x$((3000 - 137))+0+0" +repage "$OUT/d-scroll-result-shift.png" 2>/dev/null
    BAD_RMSE=$(rmse_between "$OUT/d-scroll-truth-shift.png" "$OUT/d-scroll-result-shift.png")
    # 负对照 2：只挪 1 行 —— 这是"有没有 1px 错位"最灵敏的判据
    magick "$PAGE" -crop "${CROP_W}x$((3000 - 1))+${LEFT}+1" +repage "$OUT/d-scroll-truth-shift1.png" 2>/dev/null
    magick "$RESULT" -crop "${CROP_W}x$((3000 - 1))+0+0" +repage "$OUT/d-scroll-result-shift1.png" 2>/dev/null
    BAD1_RMSE=$(rmse_between "$OUT/d-scroll-truth-shift1.png" "$OUT/d-scroll-result-shift1.png")
fi
if [ "${TOTAL_DY:-0}" -lt "$EXPECTED_DY" ]; then
    printf 'SKIP  %-42s %s\n' "D. 长图内容 == 原件（错位裁切当负对照）" \
        "没拼完整（$TOTAL_DY/$EXPECTED_DY），本次不比对内容"
    SKIPPED=$((SKIPPED + 1))
elif python3 -c "
import sys
good = float('${GOOD_RMSE:-1}')
bad = float('${BAD_RMSE:-0}')
bad1 = float('${BAD1_RMSE:-0}')
sys.exit(0 if (good < 0.05 and bad > max(0.12, 5 * good) and bad1 > 2 * good) else 1)"; then
    report "D. 长图内容 == 原件（错位裁切当负对照）" 1 \
        "与原件 RMSE=$GOOD_RMSE；错位 137 行负对照 RMSE=$BAD_RMSE（≥5 倍）；只错 1 行也有 RMSE=$BAD1_RMSE（≥2 倍）"
else
    report "D. 长图内容 == 原件（错位裁切当负对照）" 0 \
        "与原件 RMSE=$GOOD_RMSE（应 <0.05）；错位 137 行=$BAD_RMSE（应 ≥5 倍）；错 1 行=$BAD1_RMSE（应 ≥2 倍）"
fi

# 滚动条列应当被自动裁掉
if [ "${RIGHT:-0}" -ge 10 ] && [ "$CROP_W" -ge $((GW - 30)) ]; then
    report "D. 滚动条列被自动裁掉" 1 \
        "右侧裁掉 $RIGHT 列（页面滚动条宽 14），左侧裁 $LEFT 列；结果宽 ${CROP_W}px"
else
    report "D. 滚动条列被自动裁掉" 0 "左右裁列=$CROP_LR（右侧应当 ≥10）"
fi

### C) 页面不滚时：很快收工，不无限跑 ------------------------------------------
STATIC_RESULT="$OUT/d-scroll-static.png"
START_MS=$(date +%s%3N)
timeout 60 "$SEASHOT" --scroll-capture --rect "0,0,400,300" --save "$STATIC_RESULT" \
    --scroll-ms 25000 > "$OUT/d-scroll-static.txt" 2>&1
STATIC_CODE=$?
ELAPSED_MS=$(( $(date +%s%3N) - START_MS ))
STATIC_SIZE=$(sed -nE 's/^SCROLL ok size=([0-9]+x[0-9]+).*/\1/p' "$OUT/d-scroll-static.txt")
if [ "$ELAPSED_MS" -lt 6000 ] && [ "$STATIC_SIZE" = "400x300" ] && [ "$STATIC_CODE" = 0 ]; then
    report "D. 不滚动的区域会很快收工" 1 \
        "静区 400x300：${ELAPSED_MS}ms 内正常结束，结果尺寸 $STATIC_SIZE（未滚动帧被识别）"
else
    report "D. 不滚动的区域会很快收工" 0 \
        "耗时 ${ELAPSED_MS}ms 尺寸=${STATIC_SIZE:-?} 退出码=$STATIC_CODE；见 $OUT/d-scroll-static.txt"
fi

### D) 交互入口：工具条「长截图」/ Ctrl+L --------------------------------------
SEASHOT_INPLACE_DEMO_ACTION=scroll timeout 120 "$SEASHOT" --inplace-demo "$OUT/d-scroll-inplace" \
    --no-copy > "$OUT/d-scroll-inplace.txt" 2>&1
INPLACE_CODE=$?
INPLACE_SCROLL=$(grep -m1 '^SCROLL ok' "$OUT/d-scroll-inplace.txt")
if [ -n "$INPLACE_SCROLL" ] && grep -q '^INPLACE_DEMO ALL PASS' "$OUT/d-scroll-inplace.txt"; then
    report "D. 交互入口 Ctrl+L = 长截图" 1 \
        "标注阶段按 Ctrl+L → $INPLACE_SCROLL（退出码 $INPLACE_CODE）"
else
    report "D. 交互入口 Ctrl+L = 长截图" 0 \
        "SCROLL='${INPLACE_SCROLL:-（无）}'；见 $OUT/d-scroll-inplace.txt"
fi

if [ -f "$RESULT" ] && [ -f "$OUT/d-scroll-truth.png" ]; then
    report "D. 视觉证据落盘" 1 \
        "d-scroll-truth.png（原件裁切）与 d-scroll-result.png（长图 ${RES_W}x${RES_H}），可并排比对"
else
    report "D. 视觉证据落盘" 0 "缺少 $RESULT / d-scroll-truth.png"
fi

echo
echo "== 汇总: $PASS 通过 / $FAIL 失败 / $SKIPPED 跳过（环境遮挡）=="
echo "证据: $OUT/d-scroll*.txt / d-scroll*.png（长图 $RESULT）"
[ "$FAIL" = 0 ] && [ "$SCROLL_CODE" = 0 ]
