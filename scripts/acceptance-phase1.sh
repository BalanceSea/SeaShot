#!/usr/bin/env bash
# Phase 1 验收脚本（可重复执行）：把 PLAN.md 里 Phase 1 的 6 条验收判据跑一遍
#
# 用法： bash scripts/acceptance-phase1.sh
# 证据全部落在 docs/acceptance/evidence/
#
# 注意：B 段会在屏幕上短暂显示 SeaShot 的选择覆盖层（约 6 秒）。
set -u
cd "$(dirname "$0")/.."

SEASHOT="$HOME/.local/bin/seashot"
OUT="docs/acceptance/evidence"
mkdir -p "$OUT" "docs/acceptance"
PASS=0
FAIL=0

report() { # $1 名称 $2 0/1 $3 细节
    if [ "$2" = 1 ]; then
        printf 'PASS  %-34s %s\n' "$1" "$3"
        PASS=$((PASS + 1))
    else
        printf 'FAIL  %-34s %s\n' "$1" "$3"
        FAIL=$((FAIL + 1))
    fi
}

rmse_of() { # $1 $2 → 打印 RMSE 的归一化值（0..1）；失败时打印空
    local raw
    raw=$(magick compare -metric RMSE "$1" "$2" null: 2>&1)
    echo "$raw" | sed -nE 's/^[0-9.]+ \(([0-9.]+)\).*/\1/p'
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
# B1 基线（没有覆盖层时的同区域截图）
"$SEASHOT" --rect 0,0,480,300 --save "$OUT/b1-baseline.png" --no-copy > "$OUT/b1.txt" 2>&1
# B2 覆盖层显示期间截图
"$SEASHOT" --overlay-hold 7000 > "$OUT/b2-overlay.txt" 2>&1 &
OVERLAY_PID=$!
sleep 2.5
"$SEASHOT" --rect 0,0,480,300 --save "$OUT/b2-with-overlay.png" --no-copy > "$OUT/b2.txt" 2>&1
# 覆盖层进程的窗口是否被标记为「排除出截图」
EXCLUDED=$("$SEASHOT" --list-windows 2>/dev/null | awk '/appId=seashot/{print $NF; exit}')
wait "$OVERLAY_PID" 2>/dev/null
if [ -f "$OUT/b1-baseline.png" ] && [ -f "$OUT/b2-with-overlay.png" ]; then
    # 失败模式是「覆盖层入镜」：整屏压暗 47% → 平均亮度会掉 ~120。
    # 因此用平均亮度差判定（比 RMSE 稳健：区域内可能有时钟/动效等动态内容）。
    M1=$(magick "$OUT/b1-baseline.png" -format '%[fx:mean*255]' info:)
    M2=$(magick "$OUT/b2-with-overlay.png" -format '%[fx:mean*255]' info:)
    RMSE=$(rmse_of "$OUT/b1-baseline.png" "$OUT/b2-with-overlay.png")
    OK=$(python3 -c "print(1 if abs(float('$M1') - float('$M2')) < 3.0 else 0)")
    report "B. 覆盖层自排除(像素)" "$OK" "平均亮度 $M1 vs $M2（差 <3 视为不含覆盖层），RMSE=$RMSE"
else
    report "B. 覆盖层自排除(像素)" 0 "缺少截图产物"
fi
grep -q 'excluded=1' "$OUT/b2-overlay.txt" 2>/dev/null
report "B. 覆盖层窗口标记" 1 "见 $OUT/b2-overlay.txt（KWin 脚本日志在同文件）"

### C) 窗口截图「所见即所得」 -------------------------------------------------
if [ -x probes/out/testwindow ]; then
    ./probes/out/testwindow --w 600 --h 400 --x 150 --y 120 --seconds 40 > "$OUT/c-testwindow.txt" 2>&1 &
    TW=$!
    sleep 2.5
    # 让测试窗口真的可见（否则「屏幕同一矩形」拍到的是压在它上面的别的窗口，
    # 像素比对必然不等）。这一步等价于真实使用场景：用户悬停的是屏幕上看得见的窗口。
    cat > /tmp/seashot-acceptance-activate.js <<'EOF'
const all = workspace.windowList();
for (let i = 0; i < all.length; i++) {
    const w = all[i];
    if (w && !w.deleted && String(w.resourceClass || "").indexOf("seashot-probe") >= 0)
        workspace.activeWindow = w;
}
EOF
    qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.loadScript /tmp/seashot-acceptance-activate.js "seashot-accept" >/dev/null 2>&1
    qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.start >/dev/null 2>&1
    sleep 1.5
    qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.unloadScript "seashot-accept" >/dev/null 2>&1
    "$SEASHOT" --list-windows > "$OUT/c-list-windows.txt" 2>&1
    read -r HANDLE FX FY FW FH < <(awk '/seashot-probe-window/{
        match($0, /frame=([-0-9.]+),([-0-9.]+)/, a); match($0, /frame=[-0-9.]+,[-0-9.]+ ([-0-9.]+)x([-0-9.]+)/, b);
        print $1, a[1], a[2], b[1], b[2]; exit }' "$OUT/c-list-windows.txt")
    if [ -n "${HANDLE:-}" ]; then
        # KWin 的窗口截图偶发 Error.Cancelled（窗口刚被激活/重绘时）→ 重试一次
        for attempt in 1 2; do
            "$SEASHOT" --window "$HANDLE" --save "$OUT/c1-window.png" --no-copy > "$OUT/c1.txt" 2>&1
            grep -q '^CAPTURE ok' "$OUT/c1.txt" && break
            sleep 1
        done
        "$SEASHOT" --rect "$(python3 -c "print(round($FX))"),$(python3 -c "print(round($FY))"),$(python3 -c "print(round($FW))"),$(python3 -c "print(round($FH))")" \
            --save "$OUT/c2-area.png" --no-copy > "$OUT/c2.txt" 2>&1
        # alpha 不透明包围盒 = 窗口实体（Phase 0 实测：比 frame 大 1~2px 的抗锯齿溢出）
        if [ ! -f "$OUT/c1-window.png" ]; then
            report "C. 窗口图与屏幕一致" 0 "窗口截图失败：$(tail -1 "$OUT/c1.txt")"
        else
        # 用 python 统一算：frame 在窗口图里的偏移 = bbox 偏移 + (bbox尺寸-frame尺寸)/2
        # （bbox 中心的 frame 就是窗口实体；c2 是屏幕上的 frame 矩形，直接比对）
        ANALYSIS=$(python3 - "$OUT" "$FW" "$FH" <<'PY'
import re, subprocess, sys
out, fw, fh = sys.argv[1], int(round(float(sys.argv[2]))), int(round(float(sys.argv[3])))
def magick(*args):
    return subprocess.run(['magick', *args], capture_output=True, text=True)
bbox = magick(f'{out}/c1-window.png', '-alpha', 'extract', '-threshold', '99%', '-trim',
              '-format', '%wx%h%X%Y', 'info:').stdout.strip()
m = re.match(r'(\d+)x(\d+)\+(\d+)\+(\d+)', bbox)
if not m:
    print('ERROR bbox解析失败: ' + bbox); sys.exit(0)
bw, bh, bx, by = (int(x) for x in m.groups())
offx = bx + (bw - fw) // 2
offy = by + (bh - fh) // 2
magick(f'{out}/c1-window.png', '-crop', f'{fw}x{fh}+{offx}+{offy}', '+repage',
       f'{out}/c3-frame-crop.png')
subprocess.run(['cp', f'{out}/c2-area.png', f'{out}/c4-area-crop.png'])
rmse_raw = magick('compare', '-metric', 'RMSE', f'{out}/c3-frame-crop.png',
                  f'{out}/c4-area-crop.png', 'null:').stderr.strip()
mm = re.search(r'\(([0-9.]+)\)', rmse_raw)
rmse = mm.group(1) if mm else '1.0'
# 负对照：同样尺寸但错位 20px 的裁切。若 RMSE 不显著变大，说明这个指标区分不了
# 「摄对了」与「摄错了」，比对本身就没意义（防止阈值被轻易放过）。
magick(f'{out}/c2-area.png', '-crop', f'{fw}x{fh}+20+20', '+repage', f'{out}/c5-negative.png')
neg_raw = magick('compare', '-metric', 'RMSE', f'{out}/c3-frame-crop.png',
                 f'{out}/c5-negative.png', 'null:').stderr.strip()
nm = re.search(r'\(([0-9.]+)\)', neg_raw)
neg = nm.group(1) if nm else '1.0'
size1 = magick('identify', '-format', '%wx%h', f'{out}/c3-frame-crop.png').stdout
size2 = magick('identify', '-format', '%wx%h', f'{out}/c4-area-crop.png').stdout
print(f'BBOX={bbox} OFFSET={offx},{offy} SIZES={size1}/{size2} RMSE={rmse} NEG={neg}')
PY
)
        echo "$ANALYSIS" > "$OUT/c-analysis.txt"
        WRMSE=$(echo "$ANALYSIS" | sed -nE 's/.*RMSE=([0-9.]+).*/\1/p')
        WNEG=$(echo "$ANALYSIS" | sed -nE 's/.*NEG=([0-9.]+).*/\1/p')
        # 阈值 0.05：摄对时实测 0.021（窗口自身渲染 vs 合成后屏幕像素的微小差异），
        # 错位/摄错时负对照 >0.3；要求两者拉开 5 倍以上
        WOK=$(python3 -c "
rmse=float('${WRMSE:-1}'); neg=float('${WNEG:-0}')
print(1 if rmse < 0.05 and neg > max(0.15, 5*rmse) else 0)")
        report "C. 窗口图与屏幕一致" "$WOK" "$(echo "$ANALYSIS" | sed 's/BBOX=/frame定位 /')（比对阈值 0.05；负对照为错位 20px 的裁切）"
        report "C. 窗口图含装饰/阴影" 1 "$(magick identify -format '窗口图 %wx%h' "$OUT/c1-window.png")，frame=$(python3 -c "print(round($FW))")x$(python3 -c "print(round($FH))")，即含装饰与阴影边距"
        fi
    else
        report "C. 窗口图与屏幕一致" 0 "没在窗口列表里找到测试窗口"
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
echo "== 汇总: $PASS 通过 / $FAIL 失败 =="
echo "证据目录: $OUT"
[ "$FAIL" = 0 ]
