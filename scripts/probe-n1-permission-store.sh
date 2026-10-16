#!/usr/bin/env bash
# N1：看 portal 把哪个 app id 交给权限库、勾选「记住」后有没有写库
#
# 用法：bash scripts/probe-n1-permission-store.sh
#   → 弹出对话框时：选一块屏幕 → **勾上 「Allow restoring on future sessions」** → 点共享
#
# 判读（跑完看输出）：
#   1) 权限库调用里出现 string "seashot"  →  portal 认我们的 id，问题在别处
#      出现别的 id 或空字符串           →  H2 成立（app id 不对），做 N2
#      一条调用都没有                    →  这一支基本可以判定不通，按约定放弃
#   2) SetPermission 出现              →  勾选生效、已写库
#      Lookup 里已经有 seashot=yes     →  下次应该就不弹了（可以再跑一次验证）

set -u

PROBE="$HOME/.local/bin/seashot"
MON_LOG=/tmp/n1-permission-store.log
RUN_LOG=/tmp/n1-run.log
DB="$HOME/.local/share/flatpak/db/screencast"

if [ ! -x "$PROBE" ]; then
    echo "找不到 $PROBE，先跑 ./install.sh" >&2
    exit 1
fi

echo "=== 0) 跑之前的权限库状态 ==="
if [ -f "$DB" ]; then
    printf '  文件时间: %s\n' "$(stat -c '%y' "$DB")"
    printf '  seashot 条目: %s\n' "$(grep -a -c 'seashot' "$DB" 2>/dev/null || echo 0)"
else
    echo "  (权限库文件不存在)"
fi

: > "$MON_LOG"
: > "$RUN_LOG"

echo
echo "=== 1) 开始监听权限库（org.freedesktop.impl.portal.PermissionStore）==="
dbus-monitor --session "interface='org.freedesktop.impl.portal.PermissionStore'" > "$MON_LOG" 2>&1 &
MON_PID=$!
sleep 1
if ! kill -0 "$MON_PID" 2>/dev/null; then
    echo "  ⚠️ dbus-monitor 起不来，下面的判读会缺证据" >&2
fi

echo
echo "=== 2) 跑探测（现在会弹窗：选屏幕 → 勾上「Allow restoring on future sessions」→ 共享）==="
timeout 180 "$PROBE" --probe-portal-screencast > "$RUN_LOG" 2>&1
RC=$?

sleep 1
kill "$MON_PID" 2>/dev/null
wait "$MON_PID" 2>/dev/null

echo
echo "=== 3) 探测自己的输出（关键行）==="
grep -E 'PORTAL_SCREENCAST_(REGISTER|RESPONSE Start|RESULT|DIALOG_LIKELY|TOKEN_FILE|TOKEN_SAVED|PIPEWIRE_FD)' "$RUN_LOG" \
    || tail -5 "$RUN_LOG"
echo "  (退出码 $RC)"

echo
echo "=== 4) 权限库调用（app id 就在参数里）==="
if grep -q 'PermissionStore' "$MON_LOG" 2>/dev/null; then
    # 只留方法名 + 参数，去掉噪声行
    grep -E 'member=|string "|int32|boolean' "$MON_LOG" | sed 's/^[[:space:]]*//' | head -40
else
    echo "  ❌ 一次调用都没有（说明 KDE 根本没访问权限库 → 持久化没发生）"
fi

echo
echo "=== 5) 跑之后的权限库状态 ==="
if [ -f "$DB" ]; then
    printf '  文件时间: %s\n' "$(stat -c '%y' "$DB")"
    printf '  seashot 条目: %s\n' "$(grep -a -c 'seashot' "$DB" 2>/dev/null || echo 0)"
else
    echo "  (权限库文件不存在)"
fi

echo
echo "原始文件：$RUN_LOG   $MON_LOG"
