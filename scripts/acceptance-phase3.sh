#!/usr/bin/env bash
# Phase 3 验收脚本：配置界面 + 托盘菜单 + 快捷键绑定入口
#
# 用法： bash scripts/acceptance-phase3.sh
# 证据落在 docs/acceptance/evidence/
set -u
cd "$(dirname "$0")/.."

SEASHOT="$HOME/.local/bin/seashot"
# 验收会起常驻实例：关掉"首次运行自动向系统申请快捷键授权"，
# 否则系统授权对话框会弹出来干扰验收
export SEASHOT_NO_FIRST_RUN_ASK=1
OUT="docs/acceptance/evidence"
CONF="$HOME/.config/seashot/seashot.conf"
mkdir -p "$OUT" docs/acceptance
PASS=0
FAIL=0
SKIPPED=0

report() { # $1 名称 $2 0/1 $3 细节
    if [ "$2" = 1 ]; then
        printf 'PASS  %-34s %s\n' "$1" "$3"
        PASS=$((PASS + 1))
    else
        printf 'FAIL  %-34s %s\n' "$1" "$3"
        FAIL=$((FAIL + 1))
    fi
}

sni_items() { # 打印 watcher 里登记的所有 StatusNotifierItem
    qdbus6 org.kde.StatusNotifierWatcher /StatusNotifierWatcher org.freedesktop.DBus.Properties.Get \
        org.kde.StatusNotifierWatcher RegisteredStatusNotifierItems 2>/dev/null \
        | grep -oE ':1\.[0-9]+/StatusNotifierItem' | sort -u
}

echo "== Phase 3 验收 =="
echo "二进制: $SEASHOT ($(stat -c '%y' "$SEASHOT" | cut -d. -f1))"
echo

### A) 单元测试（配置界面写回 / 托盘菜单 / 配置持久化） -------------------------
if ctest --test-dir build > "$OUT/p3-unit.txt" 2>&1; then
    report "A. 单测（配置界面 + 托盘）" 1 "$(grep 'tests passed' "$OUT/p3-unit.txt" | tail -1)"
else
    report "A. 单测（配置界面 + 托盘）" 0 "见 $OUT/p3-unit.txt"
fi

### B) --print-config 键齐全 ---------------------------------------------------
"$SEASHOT" --print-config > "$OUT/p3-print-config.txt" 2>/dev/null
MISSING=""
for key in copyAfterCapture openEditorAfterCapture autoSaveAfterCapture keepShadowMargin saveDir \
           filenameTemplate hotkey hotkeyActual gifFps toastAutoHideMs trayPersistent backend; do
    grep -q "CONFIG $key=" "$OUT/p3-print-config.txt" || MISSING="$MISSING $key"
done
if [ -z "$MISSING" ]; then
    report "B. --print-config 键齐全" 1 "13 项齐备，见 $OUT/p3-print-config.txt"
else
    report "B. --print-config 键齐全" 0 "缺少:$MISSING"
fi

### C) 配置改动对新进程立即生效 ------------------------------------------------
# 用「截图后直接打开编辑器」做样本：改配置 → 新进程行为跟着变
kwriteconfig6 --file seashot.conf --group capture --key openEditorAfterCapture true 2>/dev/null
GOT=$("$SEASHOT" --print-config 2>/dev/null | grep -c 'CONFIG openEditorAfterCapture=1')
timeout 8 "$SEASHOT" --rect 0,0,160,120 --no-copy > "$OUT/p3-open-editor.txt" 2>&1 &
sleep 3
pkill -x seashot 2>/dev/null
EDITOR_OPENED=$(grep -c '^EDITOR_OPEN' "$OUT/p3-open-editor.txt" || true)
TOAST=$(grep -c '^TOAST' "$OUT/p3-open-editor.txt" || true)
kwriteconfig6 --file seashot.conf --group capture --key openEditorAfterCapture false 2>/dev/null
if [ "$GOT" = 1 ] && [ "$EDITOR_OPENED" = 1 ] && [ "$TOAST" = 0 ]; then
    report "C. 配置改后新进程生效" 1 "openEditorAfterCapture=true → EDITOR_OPEN，且不弹浮层"
else
    report "C. 配置改后新进程生效" 0 "读回=$GOT EDITOR_OPEN=$EDITOR_OPENED TOAST=$TOAST（见 $OUT/p3-open-editor.txt）"
fi

### D) 快捷键：写入 / 读回 / 换键 / 解绑（KGlobalAccel 跨进程持久化） ------------
# ⚠️ 这段会改写用户的快捷键：先记住原值，测完**恢复**（开发时就是因为没恢复，
#    把用户自己设好的 Ctrl+Alt+A 清掉了两次）。
HOTKEY_BEFORE=$(timeout 60 "$SEASHOT" --hotkey-status 2>/dev/null \
    | sed -nE 's/^HOTKEY configured=([^ ]+).*/\1/p' | head -1)
[ -n "${HOTKEY_BEFORE:-}" ] || HOTKEY_BEFORE="none"
"$SEASHOT" --set-hotkey "Ctrl+Alt+S" > "$OUT/p3-hotkey-1.txt" 2>/dev/null
H1=$("$SEASHOT" --hotkey-status 2>/dev/null | tail -1)
"$SEASHOT" --set-hotkey "Ctrl+Shift+F9" > "$OUT/p3-hotkey-2.txt" 2>/dev/null
H2=$("$SEASHOT" --hotkey-status 2>/dev/null | tail -1)
"$SEASHOT" --set-hotkey none > "$OUT/p3-hotkey-3.txt" 2>/dev/null
H3=$("$SEASHOT" --hotkey-status 2>/dev/null | tail -1)   # 先采样"解绑后的状态"再恢复原值
# 恢复原值。但如果**门户那条路已经在用**（授权过），就别再把 KGlobalAccel 绑回去 ——
# 否则系统设置里又会出现两条 SeaShot 快捷键（用户反馈过这个）。
PORTAL_AUTH=$(timeout 60 "$SEASHOT" --hotkey-portal-status 2>/dev/null \
    | grep -c '^HOTKEY_PORTAL_STATE.*authorized=1' || true)
if [ "${PORTAL_AUTH:-0}" -ge 1 ]; then
    timeout 60 "$SEASHOT" --set-hotkey none > "$OUT/p3-restore-hotkey.txt" 2>/dev/null
    echo "PORTAL_ACTIVE 恢复为 none（门户那条在生效，避免重复）" >> "$OUT/p3-restore-hotkey.txt"
else
    timeout 60 "$SEASHOT" --set-hotkey "$HOTKEY_BEFORE" > "$OUT/p3-restore-hotkey.txt" 2>/dev/null
fi
echo "$H1" > "$OUT/p3-hotkey-status.txt"
echo "$H2" >> "$OUT/p3-hotkey-status.txt"
echo "$H3" >> "$OUT/p3-hotkey-status.txt"
if echo "$H1" | grep -q 'Ctrl+Alt+S' && echo "$H2" | grep -q 'Ctrl+Shift+F9' \
    && echo "$H3" | grep -q '未绑定'; then
    report "D. 快捷键写入/读回/解绑" 1 "$(tr '\n' ' | ' < "$OUT/p3-hotkey-status.txt")"
else
    report "D. 快捷键写入/读回/解绑" 0 "$(tr '\n' ' | ' < "$OUT/p3-hotkey-status.txt")"
fi

### E) 托盘：SNI 注册 + 「常驻托盘」关掉时不注册 --------------------------------
if qdbus6 org.kde.StatusNotifierWatcher /StatusNotifierWatcher org.freedesktop.DBus.Properties.Get \
        org.kde.StatusNotifierWatcher IsStatusNotifierHostRegistered 2>/dev/null | grep -q true; then
    BEFORE=$(sni_items)
    ( "$SEASHOT" > "$OUT/p3-resident.txt" 2>&1 ) &
    sleep 3
    AFTER=$(sni_items)
    NEW=$(comm -13 <(echo "$BEFORE") <(echo "$AFTER") | head -1)
    SVC="${NEW%%/*}"
    if [ -n "$SVC" ]; then
        CAT=$(qdbus6 "$SVC" /StatusNotifierItem org.freedesktop.DBus.Properties.Get \
                  org.kde.StatusNotifierItem Category 2>/dev/null)
        STATUS=$(qdbus6 "$SVC" /StatusNotifierItem org.freedesktop.DBus.Properties.Get \
                     org.kde.StatusNotifierItem Status 2>/dev/null)
        TITLE=$(qdbus6 "$SVC" /StatusNotifierItem org.freedesktop.DBus.Properties.Get \
                    org.kde.StatusNotifierItem Title 2>/dev/null)
        echo "SNI=$NEW Category=$CAT Status=$STATUS Title=$TITLE" > "$OUT/p3-tray.txt"
        if [ "$CAT" = "ApplicationStatus" ] && [ "$STATUS" = "Active" ] && [ "$TITLE" = "SeaShot" ]; then
            report "E. 托盘 SNI 注册" 1 "Category=ApplicationStatus Status=Active Title=SeaShot（$NEW）"
        else
            report "E. 托盘 SNI 注册" 0 "$(cat "$OUT/p3-tray.txt")"
        fi
    else
        report "E. 托盘 SNI 注册" 0 "启动常驻进程后 watcher 里没有新增项（见 $OUT/p3-resident.txt）"
    fi
    pkill -x seashot 2>/dev/null
    sleep 1

    # 关掉常驻托盘后不应再注册
    kwriteconfig6 --file seashot.conf --group ui --key trayPersistent false 2>/dev/null
    B2=$(sni_items)
    ( "$SEASHOT" > "$OUT/p3-resident-notray.txt" 2>&1 ) &
    sleep 3
    A2=$(sni_items)
    NEW2=$(comm -13 <(echo "$B2") <(echo "$A2") | head -1)
    kwriteconfig6 --file seashot.conf --group ui --key trayPersistent true 2>/dev/null
    if [ -z "$NEW2" ]; then
        report "E. 关掉常驻托盘后不注册" 1 "trayPersistent=false 时 watcher 无新增项"
    else
        report "E. 关掉常驻托盘后不注册" 0 "仍注册了 $NEW2（见 $OUT/p3-resident-notray.txt）"
    fi
    pkill -x seashot 2>/dev/null
else
    printf 'SKIP  %-34s %s\n' "E. 托盘 SNI 注册" "当前桌面没有 StatusNotifierHost（watcher 未就绪），无法验证托盘"
    SKIPPED=$((SKIPPED + 1))
fi

### F) 配置界面能起来 ----------------------------------------------------------
timeout 6 "$SEASHOT" --settings > "$OUT/p3-settings.txt" 2>&1
RC=$?
if grep -q '^SETTINGS_OPENED' "$OUT/p3-settings.txt"; then
    report "F. 配置界面可启动" 1 "$(grep '^SETTINGS_OPENED' "$OUT/p3-settings.txt")（超时退出码 $RC）"
else
    report "F. 配置界面可启动" 0 "见 $OUT/p3-settings.txt"
fi
pkill -x seashot 2>/dev/null

echo
echo "== 汇总: $PASS 通过 / $FAIL 失败 / $SKIPPED 跳过 =="
echo "证据目录: $OUT"
[ "$FAIL" = 0 ]
