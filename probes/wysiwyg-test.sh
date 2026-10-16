#!/usr/bin/env bash
# Phase 0 探测：窗口截图「所见即所得」判定
#  1) 起一个内容已知、几何已知的测试窗口（自带标题栏/阴影）
#  2) 通过 KWin 脚本拿到它的 internalId 与 frameGeometry/clientGeometry
#  3) CaptureWindow 取窗口图；CaptureArea 取同区域屏幕图；逐像素比对
set -u
cd "$(dirname "$0")/.."
OUT=probes/out
BIN="$HOME/.local/bin/seashot-probe0"
DUMP=probes/kwin-probe.js
mkdir -p "$OUT"

SECONDS_TOTAL=${1:-25}
./probes/out/testwindow --w 600 --h 400 --x 150 --y 120 --seconds "$SECONDS_TOTAL" \
    > "$OUT/testwindow.log" 2>&1 &
TW=$!
sleep 2

echo "### 1) 测试窗口自报状态"
cat "$OUT/testwindow.log"

echo
echo "### 2) KWin 脚本抓窗口列表，定位测试窗口"
qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.loadScript "$PWD/$DUMP" "seashot-probe0" >/dev/null 2>&1
qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.start >/dev/null 2>&1
sleep 1.5
journalctl --user -u plasma-kwin_wayland.service --since "-1min" --no-pager 2>&1 \
    | grep 'SEASHOT_PROBE_WINDOWS' | tail -1 > "$OUT/kwin-windows-testwindow.txt"
qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.unloadScript "seashot-probe0" >/dev/null 2>&1

read -r UUID FX FY FW FH CX CY CW CH < <(python3 - <<'PY'
import json, re
txt = open('probes/out/kwin-windows-testwindow.txt').read()
ws = json.loads(re.search(r'SEASHOT_PROBE_WINDOWS (\[.*\])', txt, re.S).group(1))
for w in ws:
    if 'seashot-probe-window' in (w['resource'] or ''):
        f, c = w['frame'], w['client']
        print(w['uuid'], f['x'], f['y'], f['w'], f['h'], c['x'], c['y'], c['w'], c['h'])
        break
else:
    print("NOTFOUND 0 0 0 0 0 0 0 0")
PY
)
echo "uuid=$UUID frame=($FX,$FY,$FW,$FH) client=($CX,$CY,$CW,$CH)"
if [ "$UUID" = "NOTFOUND" ]; then
    kill $TW 2>/dev/null
    exit 1
fi

echo
echo "### 3) CaptureWindow（完整输出，含返回的几何字段）"
timeout 60 "$BIN" window "$UUID" "$OUT/10-capturewindow.png" 2>&1

echo
echo "### 4) CaptureArea（用窗口 frameGeometry 的整数化矩形）"
IX=$(python3 -c "print(round($FX))")
IY=$(python3 -c "print(round($FY))")
IW=$(python3 -c "print(round($FW))")
IH=$(python3 -c "print(round($FH))")
timeout 60 "$BIN" area "$IX" "$IY" "$IW" "$IH" "$OUT/11-capturearea-frame.png" 2>&1

echo
echo "### 5) 产物尺寸与四角/中心像素"
magick identify "$OUT/10-capturewindow.png" "$OUT/11-capturearea-frame.png" 2>&1

echo
echo "### 6) 裁剪 CaptureWindow 的「frame 区域」再与 CaptureArea 比对"
python3 - <<PY > "$OUT/12-crop.txt"
import subprocess
cw = int(subprocess.run(['magick','identify','-format','%w',"$OUT/10-capturewindow.png"],capture_output=True,text=True).stdout)
ch = int(subprocess.run(['magick','identify','-format','%h',"$OUT/10-capturewindow.png"],capture_output=True,text=True).stdout)
fw, fh = $IW, $IH
dx, dy = (cw - fw) // 2, (ch - fh) // 2
print(f"capturewindow={cw}x{ch} frame={fw}x{fh} 推得边距 dx={dx} dy={dy}")
print("CROP_X=%d CROP_Y=%d" % (dx, dy))
PY
cat "$OUT/12-crop.txt"
read -r _ _ CX0 CY0 <<< "$(grep -o 'CROP_X=[0-9-]* CROP_Y=[0-9-]*' "$OUT/12-crop.txt" | sed 's/CROP_X=//; s/ CROP_Y=/ /')"
magick "$OUT/10-capturewindow.png" -crop "$IW"x"$IH"+"$CX0"+"$CY0" +repage "$OUT/13-crop.png" 2>&1
echo "--- RMSE（裁剪后的窗口图 vs 屏幕区域图，越小越一致）---"
magick compare -metric RMSE "$OUT/13-crop.png" "$OUT/11-capturearea-frame.png" null: 2>&1 || true
echo
echo "--- 差异显著区域数（阈值 5%）---"
magick compare -metric AE -fuzz 5% "$OUT/13-crop.png" "$OUT/11-capturearea-frame.png" null: 2>&1 || true
echo
echo "### 7) 关闭测试窗口"
kill $TW 2>/dev/null
wait $TW 2>/dev/null
exit 0
