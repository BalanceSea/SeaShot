#!/usr/bin/env bash
# Phase 0 探测脚本（可重复执行，输出即证据）
# 目的：
#   1) 验证 KWin ScreenShot2 的授权是否依赖调用方已安装的 desktop 文件
#      （属性 X-KDE-DBUS-Restricted-Interfaces=org.kde.KWin.ScreenShot2），
#      以及 sycoca 刷新后 KWin 侧生效的延迟。
#   2) 刷新一次窗口列表（KWin 脚本），供窗口截取与区域截取对比使用。
set -u
cd "$(dirname "$0")/.."
OUT=probes/out
BIN="$HOME/.local/bin/seashot-probe0"
DESK="$HOME/.local/share/applications/seashot-probe0.desktop"
PROBE_DESK="probes/seashot-probe0.desktop"
mkdir -p "$OUT"

try_area() {
    echo "--- $1: shot2probe area 0 0 8 8"
    timeout 40 "$BIN" area 0 0 8 8 "$OUT/tmp-8x8.png" 2>&1 \
        | grep -E '^== (ERROR|saved|method)' | head -3
}

echo "### 1) 移除 desktop 文件后的授权状态"
rm -f "$DESK"
kbuildsycoca6 --noincremental >/dev/null 2>&1
try_area "无 desktop 文件"

echo
echo "### 2) 恢复 desktop 文件，观察授权生效延迟"
cp -f "$PROBE_DESK" "$DESK"
kbuildsycoca6 --noincremental >/dev/null 2>&1
for i in $(seq 1 12); do
    sleep 3
    r=$(timeout 40 "$BIN" area 0 0 8 8 "$OUT/tmp-8x8.png" 2>&1 | grep -E '^== (ERROR|saved)' | head -1)
    echo "attempt $i (+$((i * 3))s): $r"
    case "$r" in *saved*) break ;; esac
done

echo
echo "### 3) 重新抓取窗口列表（KWin 脚本）"
qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.loadScript \
    "$PWD/probes/kwin-probe.js" "seashot-probe0" >/dev/null 2>&1
qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.start >/dev/null 2>&1
sleep 1.5
journalctl --user -u plasma-kwin_wayland.service --since "-2min" --no-pager 2>&1 \
    | grep -E 'SEASHOT_PROBE_(WINDOWS|COUNT)' | tail -2 > "$OUT/kwin-windows.txt"
qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.unloadScript "seashot-probe0" >/dev/null 2>&1
echo "写入 $OUT/kwin-windows.txt ($(wc -l < "$OUT/kwin-windows.txt") 行)"
echo "isScriptLoaded after unload: $(qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.isScriptLoaded seashot-probe0 2>&1)"
