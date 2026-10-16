#!/usr/bin/env bash
# Phase C 验收脚本：贴图 / 钉图（layer-shell 浮窗，可拖动/缩放/右键菜单）
#
# 用法： bash scripts/acceptance-phaseC.sh
# 证据落在 docs/acceptance/evidence/
#
# 判据（全部自动判定，不依赖人工目视）：
#   1) 贴图窗口真的建起来了，且用的是 layer-shell（layerShell=1）
#   2) 拖动：贴图矩形跟着鼠标位移
#   3) 滚轮缩放：zoom 变大、尺寸跟着变大
#   4) 右键菜单：复制（剪贴板真的拿到 image/png）、保存（真的落盘）、关闭
#   5) 多张并存：两张同时存在，关一张另一张还在；全关 → 无残留（进程里 + 窗口列表里都没有）
#   6) **贴图真的合成到屏幕上**：用**另一个进程**截屏，按纯色块核对位置
#      （同一进程的 ScreenShot2 拍不到自己的 layer-shell 表面 —— ISSUE-007）
#   7) **钉图后切到全屏应用仍可见**：全屏白底参照窗口压上来之后，贴图仍然在
#   8) 就地编辑里 Ctrl+T（贴图）也能走通：结果被钉到屏幕上
#
# 注意 1：屏幕上会短暂（约 25 秒）出现贴图与一个全屏白底参照窗口，这是预期行为。
# 注意 2：A~D 段**故意**用 SEASHOT_DISABLE_SELF_EXCLUSION=1 跑两个进程。
#   原因：贴图也是 appId=seashot 的窗口，会被本项目的"自排除"一起排除出截图
#   （实测：默认设置下**任何进程**的截屏都看不到贴图，只有肉眼看得到）。
#   所以"贴图真的画在屏幕上 / 压在全屏窗口之上"这两条要用关掉自排除的观察者来验 ——
#   那才是人眼看到的样子。自排除本身是另一条独立特性（Phase 1 验收 B 段覆盖）。
set -u
cd "$(dirname "$0")/.."

SEASHOT="$HOME/.local/bin/seashot"
OUT="docs/acceptance/evidence"
PREFIX="$OUT/c-pin"
LOG="$OUT/c-pin.txt"
SHOT_A="$OUT/c-pin-screen.png"      # 全屏应用压上来之前的截屏
SHOT_B="$OUT/c-pin-on-fullscreen.png" # 全屏应用压上来之后的截屏
mkdir -p "$OUT" docs/acceptance
PASS=0
FAIL=0

report() { # $1 名称 $2 0/1 $3 细节
    if [ "$2" = 1 ]; then
        printf 'PASS  %-44s %s\n' "$1" "$3"
        PASS=$((PASS + 1))
    else
        printf 'FAIL  %-44s %s\n' "$1" "$3"
        FAIL=$((FAIL + 1))
    fi
}

# 采样一个像素，输出 "r g b"（失败输出空）
pixel_at() { # $1 png $2 x $3 y
    magick "$1" -format "%[fx:round(255*p{$2,$3}.r)] %[fx:round(255*p{$2,$3}.g)] %[fx:round(255*p{$2,$3}.b)]" info: 2>/dev/null
}

# 颜色是否接近（±8/通道）
color_near() { # $1 "r g b" $2 R $3 G $4 B
    python3 - "$1" "$2" "$3" "$4" <<'PY'
import sys
got = sys.argv[1].split()
if len(got) != 3:
    sys.exit(1)
want = [int(x) for x in sys.argv[2:5]]
sys.exit(0 if all(abs(int(g) - w) <= 8 for g, w in zip(got, want)) else 1)
PY
}

# 在贴图内部的 20%/80% 处各采一次（避开中间的 "PIN" 字样与四周边框）
check_pin_color() { # $1 png $2 x $3 y $4 w $5 h $6 R $7 G $8 B
    local png="$1" x="$2" y="$3" w="$4" h="$5" r="$6" g="$7" b="$8"
    local ok=0
    for frac in 2 8; do
        local px=$(( x + w * frac / 10 ))
        local py=$(( y + h * frac / 10 ))
        local sample
        sample=$(pixel_at "$png" "$px" "$py")
        color_near "$sample" "$r" "$g" "$b" && ok=$((ok + 1))
    done
    [ "$ok" = 2 ]
}

echo "== Phase C 验收（贴图 / 钉图）=="
echo "二进制: $SEASHOT ($(stat -c '%y' "$SEASHOT" | cut -d. -f1))"
echo

### A) 交互判据 + 两个进程的像素核对 --------------------------------------------------
rm -f "$SHOT_A" "$SHOT_B" "$LOG"
SEASHOT_DISABLE_SELF_EXCLUSION=1 SEASHOT_PIN_DEMO_HOLD=22000 timeout 90 "$SEASHOT" \
    --pin-demo "$PREFIX" > "$LOG" 2>&1 &
DEMO_PID=$!

# 等它把两张贴图摆好
for _ in $(seq 1 60); do
    grep -q '^PIN_DEMO holding=' "$LOG" 2>/dev/null && break
    sleep 0.5
done
RECTS=$(sed -nE 's/^PIN_DEMO holding=[0-9]+ pinRects=(.*)$/\1/p' "$LOG" | head -1)
PIN1=$(cut -d';' -f1 <<<"$RECTS")
PIN2=$(cut -d';' -f2 <<<"$RECTS")
read -r P1X P1Y P1W P1H < <(sed -E 's/([0-9]+),([0-9]+) ([0-9]+)x([0-9]+)/\1 \2 \3 \4/' <<<"$PIN1")
read -r P2X P2Y P2W P2H < <(sed -E 's/([0-9]+),([0-9]+) ([0-9]+)x([0-9]+)/\1 \2 \3 \4/' <<<"$PIN2")

LAYER=$(grep -c 'layerShell=1' "$LOG" || true)
CREATED=$(grep -c '^PIN_CREATED' "$LOG" || true)
if [ "$CREATED" -ge 2 ] && grep -q 'layerShell=1' "$LOG"; then
    report "C. 贴图窗口用 layer-shell 建起来了" 1 \
        "$(grep -m1 '^PIN_CREATED' "$LOG" | sed 's/^PIN_CREATED //')（layerShell 命中 $LAYER 次）"
else
    report "C. 贴图窗口用 layer-shell 建起来了" 0 "PIN_CREATED=$CREATED layerShell=$LAYER"
fi

DRAG_LINE=$(grep -m1 '^PIN_DEMO drag=' "$LOG")
DRAG=$(sed -nE 's/.*drag=([01]).*/\1/p' <<<"$DRAG_LINE")
ZOOM=$(sed -nE 's/.*zoom=([01]).*/\1/p' <<<"$DRAG_LINE")
ZOOMV=$(sed -nE 's/.*zoomValue=([0-9.]+).*/\1/p' <<<"$DRAG_LINE")
DRAGGED=$(sed -nE 's/.*rect=([^ ]+ [^ ]+).*/\1/p' <<<"$DRAG_LINE")
if [ "$DRAG" = 1 ] && [ "$ZOOM" = 1 ]; then
    report "C. 拖动 + 滚轮缩放" 1 "拖动 (+70,+50) 后矩形 $DRAGGED；滚轮后 zoom=$ZOOMV"
else
    report "C. 拖动 + 滚轮缩放" 0 "drag=$DRAG zoom=$ZOOM（$DRAG_LINE）"
fi

### B) 另一个进程截屏：贴图真的在屏幕上（按纯色块核对位置） ------------------------
sleep 0.5
SEASHOT_DISABLE_SELF_EXCLUSION=1 "$SEASHOT" --capture-screen --save "$SHOT_A" --no-copy \
    > /dev/null 2>&1
if [ -f "$SHOT_A" ] && check_pin_color "$SHOT_A" "$P1X" "$P1Y" "$P1W" "$P1H" 255 45 85 \
    && check_pin_color "$SHOT_A" "$P2X" "$P2Y" "$P2W" "$P2H" 45 255 136; then
    report "C. 贴图真的合成到屏幕上（另一进程截屏）" 1 \
        "在 $PIN1 采到贴图 1 的 #ff2d55、在 $PIN2 采到贴图 2 的 #2dff88"
else
    S1=$(pixel_at "$SHOT_A" "$((P1X + P1W / 5))" "$((P1Y + P1H / 5))")
    S2=$(pixel_at "$SHOT_A" "$((P2X + P2W / 5))" "$((P2Y + P2H / 5))")
    report "C. 贴图真的合成到屏幕上（另一进程截屏）" 0 \
        "采样=$S1 / $S2（期望 255 45 85 与 45 255 136）；截图=$SHOT_A"
fi

### C) 全屏应用压上来之后，贴图仍然可见（layer-shell LayerOverlay） ----------------
WHITE=$(mktemp /tmp/seashot-pin-white-XXXX.png)
magick -size 1920x1080 xc:white "$WHITE"
gwenview "$WHITE" > /dev/null 2>&1 &
VIEWER_PID=$!
sleep 3
cat > /tmp/seashot-pin-fullscreen.js <<'EOF'
const all = workspace.windowList();
for (let i = 0; i < all.length; i++) {
    const w = all[i];
    if (w && !w.deleted && String(w.resourceClass || "").toLowerCase().indexOf("gwenview") >= 0) {
        w.fullScreen = true;
        workspace.activeWindow = w;
    }
}
EOF
qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.loadScript /tmp/seashot-pin-fullscreen.js "seashot-pin-ref" >/dev/null 2>&1
qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.start >/dev/null 2>&1
sleep 1.5
qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.unloadScript "seashot-pin-ref" >/dev/null 2>&1
sleep 1.5
SEASHOT_DISABLE_SELF_EXCLUSION=1 "$SEASHOT" --capture-screen --save "$SHOT_B" --no-copy \
    > /dev/null 2>&1

IS_WHITE=$(pixel_at "$SHOT_B" 40 40)
if [ -f "$SHOT_B" ] && check_pin_color "$SHOT_B" "$P1X" "$P1Y" "$P1W" "$P1H" 255 45 85 \
    && check_pin_color "$SHOT_B" "$P2X" "$P2Y" "$P2W" "$P2H" 45 255 136; then
    report "C. 全屏应用前面仍然可见" 1 \
        "全屏白底参照已铺上（左上角采样 $IS_WHITE），两个贴图位置仍采到自己的颜色"
else
    S1=$(pixel_at "$SHOT_B" "$((P1X + P1W / 5))" "$((P1Y + P1H / 5))")
    report "C. 全屏应用前面仍然可见" 0 \
        "全屏参照左上角=$IS_WHITE，贴图位置采样=$S1（期望 255 45 85）；截图=$SHOT_B"
fi
kill "$VIEWER_PID" 2>/dev/null
qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.unloadScript "seashot-pin-ref" >/dev/null 2>&1
rm -f "$WHITE"

### D) 菜单交付 / 多张并存 / 关闭无残留 -------------------------------------------
wait "$DEMO_PID" 2>/dev/null
DEMO_CODE=$?
COPY_OK=$(grep -c '^PIN_COPY ok mime=image/png' "$LOG" || true)
SAVE_LINE=$(grep -m1 '^PIN_SAVE ok path=' "$LOG")
SAVE_PATH=$(sed -nE 's/^PIN_SAVE ok path=(.*)$/\1/p' <<<"$SAVE_LINE")
CLIP=$(wl-paste --list-types 2>&1 | tr '\n' ' ')
if [ "$COPY_OK" = 1 ] && [[ "$CLIP" == *image/png* ]]; then
    report "C. 右键菜单「复制」" 1 "PIN_COPY ok；另一进程 wl-paste 也看到 image/png"
else
    report "C. 右键菜单「复制」" 0 "PIN_COPY=$COPY_OK 剪贴板类型=${CLIP:-（空）}"
fi
wl-copy --clear 2>/dev/null

if [ -n "${SAVE_PATH:-}" ] && [ -f "$SAVE_PATH" ]; then
    report "C. 右键菜单「保存」" 1 \
        "PIN_SAVE ok → $(basename "$SAVE_PATH")（$(magick identify -format '%wx%h' "$SAVE_PATH" 2>/dev/null)）；验完已删除"
    rm -f "$SAVE_PATH"
else
    report "C. 右键菜单「保存」" 0 "SAVE_PATH='${SAVE_PATH:-}'"
fi

TWO=$(grep -m1 '^PIN_DEMO menu=' "$LOG" | sed -nE 's/.*twoPins=([01]).*/\1/p')
CLOSE_LINE=$(grep -m1 '^PIN_DEMO close ' "$LOG")
ONE_LEFT=$(sed -nE 's/.*oneLeft=([01]).*/\1/p' <<<"$CLOSE_LINE")
NO_RESIDUE=$(sed -nE 's/.*noResidue=([01]).*/\1/p' <<<"$CLOSE_LINE")
# 关闭后窗口列表里不该再有本应用的任何窗口（layer-shell 表面也以 appId=seashot 出现）
PINS_IN_LIST=$("$SEASHOT" --list-windows 2>/dev/null | grep -c 'appId=seashot' || true)
if [ "$TWO" = 1 ] && [ "$ONE_LEFT" = 1 ] && [ "$NO_RESIDUE" = 1 ] && [ "$PINS_IN_LIST" = 0 ]; then
    report "C. 多张并存 + 关闭无残留" 1 \
        "两张并存；关第一张后还剩 1 张；全关后进程内为空、窗口列表里也没有 appId=seashot 的窗口"
else
    report "C. 多张并存 + 关闭无残留" 0 \
        "twoPins=$TWO oneLeft=$ONE_LEFT noResidue=$NO_RESIDUE 窗口列表残留=$PINS_IN_LIST"
fi

if grep -q '^PIN_DEMO PASS' "$LOG" && [ "$DEMO_CODE" = 0 ]; then
    report "C. 汇总判定（贴图演示）" 1 "PIN_DEMO PASS（退出码 $DEMO_CODE）"
else
    report "C. 汇总判定（贴图演示）" 0 "$(grep -m1 '^PIN_DEMO ' "$LOG" | tail -1)（退出码 $DEMO_CODE）"
fi

### E) 就地编辑里 Ctrl+T 贴图 ------------------------------------------------------
INPLACE_LOG="$OUT/c-pin-inplace.txt"
SEASHOT_INPLACE_DEMO_ACTION=pin timeout 120 "$SEASHOT" --inplace-demo "$OUT/c-pin-inplace" \
    --no-copy > "$INPLACE_LOG" 2>&1
INPLACE_CODE=$?
PIN_CREATED=$(grep -m1 '^PIN_CREATED' "$INPLACE_LOG")
if [[ "$PIN_CREATED" == *"layerShell=1"* ]] && grep -q '^ANNOTATED count=' "$INPLACE_LOG"; then
    report "C. 就地编辑 Ctrl+T = 贴图" 1 \
        "标注后的结果被钉到屏幕上：$(sed 's/^PIN_CREATED //; s/ 共.*//' <<<"$PIN_CREATED")（退出码 $INPLACE_CODE）"
else
    report "C. 就地编辑 Ctrl+T = 贴图" 0 "PIN_CREATED='${PIN_CREATED:-（无）}'；见 $INPLACE_LOG"
fi

if [ -f "$SHOT_A" ] && [ -f "$SHOT_B" ]; then
    report "C. 视觉证据落盘" 1 "c-pin-screen.png（贴图）与 c-pin-on-fullscreen.png（全屏参照之上）"
else
    report "C. 视觉证据落盘" 0 "缺少截图产物"
fi

echo
echo "== 汇总: $PASS 通过 / $FAIL 失败 =="
echo "证据: $LOG、$INPLACE_LOG 与 $OUT/c-pin-*.png"
[ "$FAIL" = 0 ] && [ "$DEMO_CODE" = 0 ]
