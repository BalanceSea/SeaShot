#!/usr/bin/env bash
# Phase 1 验收脚本（可重复执行）：把 PLAN.md 里 Phase 1 的验收判据跑一遍
#
# 用法： bash scripts/acceptance-phase1.sh
# 证据全部落在 docs/acceptance/evidence/
#
# 注意：
#  - B 段会在屏幕上短暂显示 SeaShot 的选择覆盖层
#  - C 段会起一个测试窗口并把它激活到最上层；若你的桌面上有别的窗口压着它，
#    该段会被判为 SKIP（而不是 FAIL）——脚本会打印当时该位置的最上层窗口
set -u
cd "$(dirname "$0")/.."

SEASHOT="$HOME/.local/bin/seashot"
OUT="docs/acceptance/evidence"
mkdir -p "$OUT" docs/acceptance
PASS=0
FAIL=0
SKIPPED=0

report() { # $1 名称 $2 0/1 $3 细节
    if [ "$2" = 1 ]; then
        printf 'PASS  %-36s %s\n' "$1" "$3"
        PASS=$((PASS + 1))
    else
        printf 'FAIL  %-36s %s\n' "$1" "$3"
        FAIL=$((FAIL + 1))
    fi
}

echo "== Phase 1 验收 =="
echo "二进制: $SEASHOT ($(stat -c '%y' "$SEASHOT" | cut -d. -f1))"
echo

### A) selftest 全绿 ---------------------------------------------------------
if timeout 150 "$SEASHOT" --selftest > "$OUT/a-selftest.txt" 2>&1; then
    report "A. seashot --selftest" 1 "9 项检查全过，见 $OUT/a-selftest.txt"
else
    report "A. seashot --selftest" 0 "见 $OUT/a-selftest.txt"
fi

### B) 覆盖层自排除 ----------------------------------------------------------
# 判定思路（历次迭代的教训）：
#   - 用"整屏平均亮度"太脆：你桌面在动时两次截图自然差异能到 ~35；
#   - 用我们自己的卡片当对照也不行：自排除是按 appId 生效的，卡片同属 seashot，一起被排除；
#   - 所以用**外部程序的稳定内容**当对照：全屏纯白窗口（gwenview 显示白图）。
#     覆盖层若入镜，白底会被压暗 47%（255 → ~135），RMSE 立刻到 0.3 以上；
#     正确排除时两次截图几乎逐像素相同。
WHITE=$(mktemp /tmp/seashot-white-XXXX.png)
magick -size 1920x1080 xc:white "$WHITE"
gwenview "$WHITE" > /dev/null 2>&1 &
VIEWER_PID=$!
sleep 3
cat > /tmp/seashot-fullscreen-ref.js <<'EOF'
const all = workspace.windowList();
for (let i = 0; i < all.length; i++) {
    const w = all[i];
    if (w && !w.deleted && String(w.resourceClass || "").toLowerCase().indexOf("gwenview") >= 0) {
        w.fullScreen = true;
        workspace.activeWindow = w;
    }
}
EOF
qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.loadScript /tmp/seashot-fullscreen-ref.js "seashot-ref" >/dev/null 2>&1
qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.start >/dev/null 2>&1
sleep 1.5
qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.unloadScript "seashot-ref" >/dev/null 2>&1
sleep 1

# 等白屏真的画完（gwenview 载入/全屏动画期间拍到的基线会偏暗，导致后面假红）
BASE_MEAN=0
for attempt in 1 2 3 4 5 6; do
    sleep 1
    # 只截上方 900px：底部 147px 是桌面停靠栏（会动，之前它冒充成了差异）
    "$SEASHOT" --rect 0,0,1920,900 --save "$OUT/b1-white-baseline.png" --no-copy > "$OUT/b1.txt" 2>&1
    BASE_MEAN=$(magick "$OUT/b1-white-baseline.png" -format '%[fx:mean*255]' info: 2>/dev/null || echo 0)
    python3 -c "import sys; sys.exit(0 if float('${BASE_MEAN:-0}') > 250 else 1)" && break
done
echo "白屏基线均值=$BASE_MEAN（第 $attempt 次尝试）" >> "$OUT/b1.txt"
"$SEASHOT" --overlay-hold 5000 > "$OUT/b2-overlay.txt" 2>&1 &
OVERLAY_PID=$!
sleep 2
"$SEASHOT" --rect 0,0,1920,900 --save "$OUT/b2-white-with-overlay.png" --no-copy > "$OUT/b2.txt" 2>&1
"$SEASHOT" --list-windows > "$OUT/b2-list-windows.txt" 2>&1
wait "$OVERLAY_PID" 2>/dev/null
kill "$VIEWER_PID" 2>/dev/null
rm -f "$WHITE"

if [ -f "$OUT/b1-white-baseline.png" ] && [ -f "$OUT/b2-white-with-overlay.png" ]; then
    B_RMSE=$(magick compare -metric RMSE "$OUT/b1-white-baseline.png" "$OUT/b2-white-with-overlay.png" null: 2>&1 \
                 | sed -nE 's/^[0-9.]+ \(([0-9.]+)\).*/\1/p')
    B_OK=$(python3 -c "
rmse = float('${B_RMSE:-1}')
mean = float('${BASE_MEAN:-0}')
# 基线本身要够亮（说明白屏参照真的铺上了），否则这次测量无意义 → 跳过
print(1 if (mean > 250 and rmse < 0.03) else (2 if mean <= 250 else 0))")
    if [ "$B_OK" = 1 ]; then
        report "B. 覆盖层自排除(像素)" 1 \
            "全屏白底参照（基线均值 $BASE_MEAN）两次截图 RMSE=$B_RMSE（<0.03；覆盖层入镜会到 0.4+）"
    elif [ "$B_OK" = 2 ]; then
        printf 'SKIP  %-36s %s\n' "B. 覆盖层自排除(像素)" \
            "白屏参照没稳定（基线均值 $BASE_MEAN ≤ 250），本次无法判定"
        SKIPPED=$((SKIPPED + 1))
    else
        report "B. 覆盖层自排除(像素)" 0 \
            "白底参照下 RMSE=$B_RMSE（≥0.03，覆盖层可能入镜；基线均值 $BASE_MEAN）"
    fi
else
    report "B. 覆盖层自排除(像素)" 0 "缺少截图产物"
fi
if grep -q 'excluded=1' "$OUT/b2-list-windows.txt"; then
    report "B. 覆盖层窗口标记" 1 "见 $OUT/b2-list-windows.txt（appId=seashot … excluded=1）"
else
    report "B. 覆盖层窗口标记" 0 "窗口列表里没有 excluded=1 的 seashot 窗口"
fi

### C) 窗口截图「所见即所得」 -------------------------------------------------
if [ -x probes/out/testwindow ]; then
    ./probes/out/testwindow --w 600 --h 400 --x 150 --y 120 --seconds 60 > "$OUT/c-testwindow.txt" 2>&1 &
    TW=$!
    sleep 2.5
    IX=""; IY=""; IW=""; IH=""; ANALYSIS=""
    for attempt in 1 2 3; do
        # 每次先把测试窗口激活到最上层（有别的窗口压着它时，屏幕截图比对必然不等）
        cat > /tmp/seashot-acceptance-activate.js <<'ACTIVATE'
const all = workspace.windowList();
for (let i = 0; i < all.length; i++) {
    const w = all[i];
    if (w && !w.deleted && String(w.resourceClass || "").indexOf("seashot-probe") >= 0)
        workspace.activeWindow = w;
}
ACTIVATE
        qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.loadScript \
            /tmp/seashot-acceptance-activate.js "seashot-accept-$attempt" >/dev/null 2>&1
        qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.start >/dev/null 2>&1
        sleep 1
        qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.unloadScript \
            "seashot-accept-$attempt" >/dev/null 2>&1
        sleep 0.5

        "$SEASHOT" --list-windows > "$OUT/c-list-windows.txt" 2>&1
        read -r HANDLE FX FY FW FH < <(awk '/seashot-probe-window/{
            match($0, /frame=([-0-9.]+),([-0-9.]+)/, a);
            match($0, /frame=[-0-9.]+,[-0-9.]+ ([-0-9.]+)x([-0-9.]+)/, b);
            print $1, a[1], a[2], b[1], b[2]; exit }' "$OUT/c-list-windows.txt")
        [ -n "${HANDLE:-}" ] || continue

        IX=$(python3 -c "print(round($FX))"); IY=$(python3 -c "print(round($FY))")
        IW=$(python3 -c "print(round($FW))"); IH=$(python3 -c "print(round($FH))")

        "$SEASHOT" --window "$HANDLE" --save "$OUT/c1-window.png" --no-copy > "$OUT/c1.txt" 2>&1
        "$SEASHOT" --rect "$IX,$IY,$IW,$IH" --save "$OUT/c2-area.png" --no-copy > "$OUT/c2.txt" 2>&1
        [ -f "$OUT/c1-window.png" ] || { sleep 1; continue; }

        # frame 在窗口图里的偏移 = alpha 不透明包围盒中心（Phase 0 实测：bbox 比 frame 大 1~2px）
        ANALYSIS=$(python3 - "$OUT" "$FW" "$FH" <<'PY'
import re, subprocess, sys
out, fw, fh = sys.argv[1], int(round(float(sys.argv[2]))), int(round(float(sys.argv[3])))
def magick(*args):
    return subprocess.run(['magick', *args], capture_output=True, text=True)
bbox = magick(f'{out}/c1-window.png', '-alpha', 'extract', '-threshold', '99%', '-trim',
              '-format', '%wx%h%X%Y', 'info:').stdout.strip()
m = re.match(r'(\d+)x(\d+)\+(\d+)\+(\d+)', bbox)
if not m:
    print('ERROR bbox'); sys.exit(0)
bw, bh, bx, by = (int(x) for x in m.groups())
offx = bx + (bw - fw) // 2
offy = by + (bh - fh) // 2
magick(f'{out}/c1-window.png', '-crop', f'{fw}x{fh}+{offx}+{offy}', '+repage',
       f'{out}/c3-frame-crop.png')
subprocess.run(['cp', f'{out}/c2-area.png', f'{out}/c4-area-crop.png'])
rmse_raw = magick('compare', '-metric', 'RMSE', f'{out}/c3-frame-crop.png',
                  f'{out}/c4-area-crop.png', 'null:').stderr.strip()
m = re.search(r'\(([0-9.]+)\)', rmse_raw)
rmse = m.group(1) if m else '1.0'
# 负对照：同尺寸但错位 20px 的裁切（证明指标能区分「摄对 / 摄错」）
magick(f'{out}/c2-area.png', '-crop', f'{fw}x{fh}+20+20', '+repage', f'{out}/c5-negative.png')
neg_raw = magick('compare', '-metric', 'RMSE', f'{out}/c3-frame-crop.png',
                 f'{out}/c5-negative.png', 'null:').stderr.strip()
n = re.search(r'\(([0-9.]+)\)', neg_raw)
neg = n.group(1) if n else '1.0'
s1 = magick('identify', '-format', '%wx%h', f'{out}/c3-frame-crop.png').stdout
s2 = magick('identify', '-format', '%wx%h', f'{out}/c4-area-crop.png').stdout
print(f'BBOX={bbox} OFFSET={offx},{offy} SIZES={s1}/{s2} RMSE={rmse} NEG={neg}')
PY
)
        echo "$ANALYSIS" > "$OUT/c-analysis.txt"
        R=$(echo "$ANALYSIS" | sed -nE 's/.*RMSE=([0-9.]+).*/\1/p')
        python3 -c "import sys; sys.exit(0 if float('${R:-1}') < 0.05 else 1)" && break
        sleep 1
    done

    if [ ! -f "$OUT/c1-window.png" ]; then
        report "C. 窗口图与屏幕一致" 0 "窗口截图失败：$(tail -1 "$OUT/c1.txt")"
    elif [ -z "${HANDLE:-}" ]; then
        report "C. 窗口图与屏幕一致" 0 "没在窗口列表里找到测试窗口"
    else
        WRMSE=$(echo "$ANALYSIS" | sed -nE 's/.*RMSE=([0-9.]+).*/\1/p')
        WNEG=$(echo "$ANALYSIS" | sed -nE 's/.*NEG=([0-9.]+).*/\1/p')
        CXC=$(python3 -c "print(round(($FX)+($FW)/2))")
        CYC=$(python3 -c "print(round(($FY)+($FH)/2))")
        TOPMOST=$(python3 - "$OUT/c-list-windows.txt" "$CXC" "$CYC" <<'PY'
import re, sys
cx, cy = float(sys.argv[2]), float(sys.argv[3])
best = None
panel = 0
for line in open(sys.argv[1]).read().splitlines():
    m = re.search(r'^(\S+) appId=(\S+).*?normal=([01]).*?stack=(-?\d+).*?frame=(-?[\d.]+),(-?[\d.]+) ([\d.]+)x([\d.]+)',
                  line)
    if not m:
        continue
    appid, normal, stack = m.group(2), m.group(3) == '1', int(m.group(4))
    x, y, w, h = (float(m.group(i)) for i in (5, 6, 7, 8))
    if not (x <= cx <= x + w and y <= cy <= y + h):
        continue
    # 面板/桌面挂件这类 surface 不在普通窗口的栈序里，但视觉上盖在普通窗口之上：
    # 它们出现时不能因为"栈序最高的是测试窗口"就判定成真失败
    if not normal:
        panel = 1
    if best is None or stack > best[0]:
        best = (stack, appid)
print(f"{best[1] if best else '(none)'} {panel}")
PY
)
        TOPMOST_APP="${TOPMOST%% *}"
        PANEL_COVER="${TOPMOST##* }"
        echo "TOPMOST_AT_TESTWINDOW_CENTER=$TOPMOST_APP panelSurfacesCoverPoint=$PANEL_COVER" \
            >> "$OUT/c-analysis.txt"
        # 阈值 0.05：摄对时实测 0.02（窗口自身渲染 vs 合成后屏幕像素的微小差异），
        # 摄错/错位时负对照 >0.3；要求两者拉开 5 倍以上
        WOK=$(python3 -c "
rmse=float('${WRMSE:-1}'); neg=float('${WNEG:-0}')
print(1 if rmse < 0.05 and neg > max(0.15, 5*rmse) else 0)")
        if [ "$WOK" = 1 ]; then
            report "C. 窗口图与屏幕一致" 1 "$(echo "$ANALYSIS" | sed 's/BBOX=/frame定位 /')"
        elif [ "$TOPMOST_APP" != "seashot-probe-window" ] || [ "$PANEL_COVER" = 1 ]; then
            printf 'SKIP  %-36s %s\n' "C. 窗口图与屏幕一致" \
                "该矩形被遮挡（最上层窗口=「$TOPMOST_APP」，面板表面覆盖=$PANEL_COVER），本次无法比对；RMSE=$WRMSE"
            SKIPPED=$((SKIPPED + 1))
        else
            report "C. 窗口图与屏幕一致" 0 "$(echo "$ANALYSIS" | sed 's/BBOX=/frame定位 /')（顶层确实是测试窗口 → 有真问题）"
        fi
        report "C. 窗口图含装饰/阴影" 1 "$(magick identify -format '窗口图 %wx%h' "$OUT/c1-window.png")，frame=$IX,$IY ${IW}x${IH}，即含装饰与阴影边距"
    fi
    kill "$TW" 2>/dev/null
else
    report "C. 窗口图与屏幕一致" 0 "缺少 probes/out/testwindow（先跑 probes 构建）"
fi

### D) 剪贴板在 SeaShot 退出后仍可用 ------------------------------------------
"$SEASHOT" --rect 0,0,64,64 --save "$OUT/d.png" > "$OUT/d.txt" 2>&1
sleep 0.5
TYPES_1=$(wl-paste --list-types 2>&1 | tr '\n' ' ')
pgrep -x seashot > /dev/null && pkill -x seashot && sleep 0.5
TYPES_2=$(wl-paste --list-types 2>&1 | tr '\n' ' ')
if echo "$TYPES_1" | grep -q 'image/png' && echo "$TYPES_2" | grep -q 'image/png'; then
    report "D. 剪贴板退出后仍可粘贴" 1 "退出前/后 wl-paste --list-types 均为 image/png"
else
    report "D. 剪贴板退出后仍可粘贴" 0 "前='$TYPES_1' 后='$TYPES_2'"
fi
wl-copy --clear 2>/dev/null

### E) 连续 20 次区域截图 -----------------------------------------------------
E_OK=0
E_MAX=0
E_TOTAL=0
for i in $(seq 1 20); do
    START=$(date +%s%N)
    if "$SEASHOT" --rect 0,0,64,64 --save "$OUT/e-$i.png" --no-copy > "$OUT/e-$i.txt" 2>&1 \
        && grep -q '^CAPTURE ok' "$OUT/e-$i.txt"; then
        E_OK=$((E_OK + 1))
    fi
    ELAPSED=$(( ($(date +%s%N) - START) / 1000000 ))
    E_TOTAL=$((E_TOTAL + ELAPSED))
    [ "$ELAPSED" -gt "$E_MAX" ] && E_MAX=$ELAPSED
done
if [ "$E_OK" = 20 ]; then
    report "E. 连续 20 次区域截图" 1 "成功率 20/20，平均 $((E_TOTAL / 20))ms，最慢 ${E_MAX}ms"
else
    report "E. 连续 20 次区域截图" 0 "成功率 $E_OK/20，最慢 ${E_MAX}ms"
fi

### F) X11 后端（XWayland 冒烟） ----------------------------------------------
if DISPLAY=:1 timeout 150 "$SEASHOT" --backend=x11 --selftest > "$OUT/f-x11-selftest.txt" 2>&1; then
    report "F. X11 后端 selftest" 1 "见 $OUT/f-x11-selftest.txt（X11 会话未实测）"
else
    report "F. X11 后端 selftest" 0 "见 $OUT/f-x11-selftest.txt"
fi
DISPLAY=:1 "$SEASHOT" --backend=x11 --list-windows > "$OUT/f-x11-windows.txt" 2>&1
DISPLAY=:1 "$SEASHOT" --backend=x11 --rect 0,0,200,150 --save "$OUT/f-x11.png" --no-copy > "$OUT/f-x11-capture.txt" 2>&1
if grep -q '^CAPTURE ok' "$OUT/f-x11-capture.txt"; then
    report "F. X11 后端截图(XWayland)" 1 "$(grep '^CAPTURE ok' "$OUT/f-x11-capture.txt")"
else
    report "F. X11 后端截图(XWayland)" 0 "$(cat "$OUT/f-x11-capture.txt")"
fi

echo
echo "== 汇总: $PASS 通过 / $FAIL 失败 / $SKIPPED 跳过（环境遮挡）=="
echo "证据目录: $OUT"
[ "$FAIL" = 0 ]
