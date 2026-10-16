#!/usr/bin/env bash
# Phase 0 探测（最后一次）：覆盖层的两类自排除路线
#  A) 覆盖层可见时用 CaptureWindow 截目标窗口 —— 检验是否不受覆盖层影响
#  B) 用 KWin 脚本把覆盖层的 excludeFromCapture 置 true 后 CaptureArea —— 检验
#     该属性是否真能让覆盖层从截图里消失（这是「不隐藏覆盖层也能截区域」的前提）
set -u
cd "$(dirname "$0")/.."
OUT=probes/out
BIN="$HOME/.local/bin/seashot-probe0"
mkdir -p "$OUT"

dump_windows() {
    qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.loadScript "$PWD/probes/kwin-probe.js" "seashot-probe0" >/dev/null 2>&1
    qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.start >/dev/null 2>&1
    sleep 1.5
    journalctl --user -u plasma-kwin_wayland.service --since "-1min" --no-pager 2>&1 \
        | grep 'SEASHOT_PROBE_WINDOWS' | tail -1 > "$OUT/kwin-windows-both.txt"
    qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.unloadScript "seashot-probe0" >/dev/null 2>&1
}

pick() { # $1 = resource 子串；输出 uuid fx fy fw fh
    python3 - "$1" <<'PY'
import json, re, sys
needle = sys.argv[1]
txt = open('probes/out/kwin-windows-both.txt').read()
ws = json.loads(re.search(r'SEASHOT_PROBE_WINDOWS (\[.*\])', txt, re.S).group(1))
for w in ws:
    if needle in (w['resource'] or ''):
        f = w['frame']
        print(w['uuid'], f['x'], f['y'], f['w'], f['h'])
        break
PY
}

kill_all() { kill $TW $OV 2>/dev/null; }

./probes/out/testwindow --w 600 --h 400 --x 150 --y 120 --seconds 55 > "$OUT/testwindow6.log" 2>&1 &
TW=$!
./probes/build/overlayprobe --seconds 55 > "$OUT/overlay4.log" 2>&1 &
OV=$!
sleep 3
dump_windows
read -r UUID FX FY FW FH < <(pick seashot-probe-window)
read -r OVUUID OFX OFY OFW OFH < <(pick overlayprobe)
echo "测试窗口: uuid=$UUID frame=($FX,$FY,$FW,$FH)"
echo "覆盖层:   uuid=$OVUUID frame=($OFX,$OFY,$OFW,$OFH)"
echo "覆盖层程序日志: $(tr '\n' ' | ' < "$OUT/overlay4.log")"

echo
echo "### A) 覆盖层可见时 CaptureWindow(测试窗口)"
timeout 40 "$BIN" window "$UUID" "$OUT/80-window-with-overlay.png" 2>&1 | grep -E 'saved|ERROR'
echo "meanRGB = $(magick "$OUT/80-window-with-overlay.png" -format '%[fx:mean.r*255],%[fx:mean.g*255],%[fx:mean.b*255]' info: 2>&1)"
echo "对照（无覆盖层时同一窗口）：$(magick "$OUT/31-client.png" -format '%[fx:mean.r*255],%[fx:mean.g*255],%[fx:mean.b*255]' info: 2>&1)"
echo "RMSE(vs 无覆盖层时的窗口图) = $(magick compare -metric RMSE "$OUT/80-window-with-overlay.png" "$OUT/31-client.png" null: 2>&1)"

echo
echo "### B) 覆盖层可见时 CaptureArea(窗口矩形) —— 预期被品红覆盖"
IX=$(python3 -c "print(round($FX))"); IY=$(python3 -c "print(round($FY))")
IW=$(python3 -c "print(round($FW))"); IH=$(python3 -c "print(round($FH))")
timeout 40 "$BIN" area "$IX" "$IY" "$IW" "$IH" "$OUT/81-area-with-overlay.png" 2>&1 | grep -E 'saved|ERROR'
echo "meanRGB = $(magick "$OUT/81-area-with-overlay.png" -format '%[fx:mean.r*255],%[fx:mean.g*255],%[fx:mean.b*255]' info: 2>&1)"

echo
echo "### C) 给覆盖层设 excludeFromCapture=true（用真实 resourceClass=overlayprobe）"
cat > /tmp/kwin-efc-on.js <<'EOF'
function on() {
    const all = workspace.windowList();
    let hits = 0;
    for (let i = 0; i < all.length; i++) {
        const w = all[i];
        if (!w || w.deleted) continue;
        if (String(w.resourceClass || "").indexOf("overlayprobe") < 0) continue;
        let before = "n/a", after = "n/a", err = "";
        try { before = String(w.excludeFromCapture); } catch (e) { err += "read:" + e; }
        try { w.excludeFromCapture = true; after = String(w.excludeFromCapture); } catch (e) { err += " write:" + e; }
        print("SEASHOT_EFC uuid=" + w.internalId + " before=" + before + " after=" + after + " err=" + err);
        hits++;
    }
    print("SEASHOT_EFC hits=" + hits);
}
on();
EOF
qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.loadScript "/tmp/kwin-efc-on.js" "seashot-efc2" >/dev/null 2>&1
qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.start >/dev/null 2>&1
sleep 1.5
journalctl --user -u plasma-kwin_wayland.service --since "-30s" --no-pager 2>&1 | grep SEASHOT_EFC | tail -3
qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.unloadScript "seashot-efc2" >/dev/null 2>&1
sleep 1
timeout 40 "$BIN" area "$IX" "$IY" "$IW" "$IH" "$OUT/82-area-efc.png" 2>&1 | grep -E 'saved|ERROR'
echo "meanRGB = $(magick "$OUT/82-area-efc.png" -format '%[fx:mean.r*255],%[fx:mean.g*255],%[fx:mean.b*255]' info: 2>&1)"

echo
echo "### D) 复位（避免留下改动）并收尾"
cat > /tmp/kwin-efc-off.js <<'EOF'
const all = workspace.windowList();
for (let i = 0; i < all.length; i++) {
    const w = all[i];
    if (!w || w.deleted) continue;
    if (String(w.resourceClass || "").indexOf("overlayprobe") < 0) continue;
    try { w.excludeFromCapture = false; print("SEASHOT_EFC_OFF " + w.internalId + " -> " + String(w.excludeFromCapture)); } catch (e) { print("SEASHOT_EFC_OFF_ERR " + e); }
}
EOF
qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.loadScript "/tmp/kwin-efc-off.js" "seashot-efc3" >/dev/null 2>&1
qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.start >/dev/null 2>&1
sleep 1.5
journalctl --user -u plasma-kwin_wayland.service --since "-30s" --no-pager 2>&1 | grep SEASHOT_EFC_OFF | tail -2
qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.unloadScript "seashot-efc3" >/dev/null 2>&1
kill_all
wait 2>/dev/null
exit 0
