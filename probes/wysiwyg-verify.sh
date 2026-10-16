#!/usr/bin/env bash
# Phase 0 探测：判定「所见即所得」窗口截图路线是否成立
#
# 做三件事：
#  1) 用受控测试窗口（已知 600x400 内容 + KWin 标题栏）测 CaptureWindow 的
#     include-decoration 选项到底给了什么；
#  2) 用测试窗口正中的品红定位块，算出「frame 在返回图像中的偏移」；
#  3) 用该偏移反推屏幕坐标矩形，CaptureArea 取同一矩形并与窗口图逐像素比对。
#     若 RMSE ≈ 0，说明 include-decoration 的窗口图 == 屏幕上所见（含阴影/标题栏）。
set -u
cd "$(dirname "$0")/.."
OUT=probes/out
BIN="$HOME/.local/bin/seashot-probe0"
mkdir -p "$OUT"

./probes/out/testwindow --w 600 --h 400 --x 150 --y 120 --seconds 120 > "$OUT/testwindow4.log" 2>&1 &
TW=$!
sleep 2.5

dump() {
    qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.loadScript "$PWD/probes/kwin-probe.js" "seashot-probe0" >/dev/null 2>&1
    qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.start >/dev/null 2>&1
    sleep 1.5
    journalctl --user -u plasma-kwin_wayland.service --since "-1min" --no-pager 2>&1 \
        | grep 'SEASHOT_PROBE_WINDOWS' | tail -1 > "$OUT/kwin-windows-testwindow.txt"
    qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.unloadScript "seashot-probe0" >/dev/null 2>&1
}
dump
read -r UUID FX FY FW FH CX CY CW CH ACT < <(python3 - <<'PY'
import json, re
txt = open('probes/out/kwin-windows-testwindow.txt').read()
ws = json.loads(re.search(r'SEASHOT_PROBE_WINDOWS (\[.*\])', txt, re.S).group(1))
for w in ws:
    if 'seashot-probe-window' in (w['resource'] or ''):
        f, c = w['frame'], w['client']
        print(w['uuid'], f['x'], f['y'], f['w'], f['h'], c['x'], c['y'], c['w'], c['h'], w['active'])
        break
PY
)
echo "uuid=$UUID frame=($FX,$FY,$FW,$FH) client=($CX,$CY,$CW,$CH) active=$ACT"

echo
echo "### 把测试窗口激活到最上层（保证区域截取看到的是它）"
cat > /tmp/kwin-activate-probe.js <<EOF
const all = workspace.windowList();
for (let i = 0; i < all.length; i++) {
    const w = all[i];
    if (w && !w.deleted && String(w.resourceClass || "").indexOf("seashot-probe") >= 0) {
        workspace.activeWindow = w;
        print("SEASHOT_ACTIVATED " + w.internalId);
    }
}
EOF
qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.loadScript "/tmp/kwin-activate-probe.js" "seashot-act0" >/dev/null 2>&1
qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.start >/dev/null 2>&1
sleep 1.5
qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.unloadScript "seashot-act0" >/dev/null 2>&1
journalctl --user -u plasma-kwin_wayland.service --since "-40s" --no-pager 2>&1 | grep SEASHOT_ACTIVATED | tail -2

echo
echo "### 截取（client-only / include-decoration）"
timeout 30 "$BIN" window "$UUID" "$OUT/31-client.png" 2>&1 | grep -E 'saved'
timeout 30 "$BIN" window "$UUID" "$OUT/30-decor.png" "include-decoration=true" 2>&1 | grep -E 'saved'

python3 - "$OUT" "$FX" "$FY" "$FW" "$FH" "$CX" "$CY" "$CW" "$CH" <<'PY'
import subprocess, sys
out, FX, FY, FW, FH, CX, CY, CW, CH = sys.argv[1], *map(float, sys.argv[2:10])
DW = int(subprocess.run(['magick','identify','-format','%w',f'{out}/30-decor.png'],capture_output=True,text=True).stdout)
DH = int(subprocess.run(['magick','identify','-format','%h',f'{out}/30-decor.png'],capture_output=True,text=True).stdout)

# 在 decor 图里找品红定位块（测试窗口客户区正中 10x10）
txt = subprocess.run(['magick', f'{out}/30-decor.png', '-crop', f'{DW}x{DH}+0+0', 'txt:-'],
                     capture_output=True, text=True).stdout
xs, ys = [], []
for line in txt.splitlines()[1:]:
    if '#FF00FF' in line.upper():
        pos = line.split(':')[0]
        x, y = pos.split(',')
        xs.append(int(x)); ys.append(int(y))
if not xs:
    print("品红定位块未找到 —— 无法定位")
    sys.exit(1)
bx, by = min(xs), min(ys)
bw, bh = max(xs) - bx + 1, max(ys) - by + 1
print(f"decor 图 {DW}x{DH}；品红块 bbox=({bx},{by},{bw},{bh})")
# 品红块在客户区的中心 = (CW/2, CH/2)
client_x = bx + bw / 2 - CW / 2
client_y = by + bh / 2 - CH / 2
frame_x = client_x - (CX - FX)
frame_y = client_y - (CY - FY)
print(f"推得：client 偏移=({client_x:g},{client_y:g})  frame 偏移=({frame_x:g},{frame_y:g})")
print(f"边距：left={frame_x:g} top={frame_y:g} right={DW - frame_x - FW:g} bottom={DH - frame_y - FH:g}")
ax, ay = FX - frame_x, FY - frame_y
print(f"屏幕矩形：IX={round(ax)} IY={round(ay)} IW={DW} IH={DH}")
with open(f'{out}/34-area.txt', 'w') as f:
    f.write(f"IX={round(ax)} IY={round(ay)} IW={DW} IH={DH}\n")
PY
cat "$OUT/34-area.txt"
read -r IX IY IW IH < <(sed 's/[A-Z]*=//g' "$OUT/34-area.txt")
timeout 30 "$BIN" area "$IX" "$IY" "$IW" "$IH" "$OUT/32-area-decor.png" 2>&1 | grep -E 'saved|ERROR'
echo "RMSE(decor 窗口图 vs 同矩形屏幕图) = $(magick compare -metric RMSE "$OUT/30-decor.png" "$OUT/32-area-decor.png" null: 2>&1)"
echo "AE fuzz2%                       = $(magick compare -metric AE -fuzz 2% "$OUT/30-decor.png" "$OUT/32-area-decor.png" null: 2>&1)"
CLX=$(python3 -c "print(round($CX - ($IX)))")
CLY=$(python3 -c "print(round($CY - ($IY)))")
magick "$OUT/32-area-decor.png" -crop "${CW%.*}x${CH%.*}+$CLX+$CLY" +repage "$OUT/35-area-client-crop.png" 2>&1
echo "RMSE(client 窗口图 vs 从屏幕图裁出的客户区) = $(magick compare -metric RMSE "$OUT/31-client.png" "$OUT/35-area-client-crop.png" null: 2>&1)"
kill $TW 2>/dev/null
