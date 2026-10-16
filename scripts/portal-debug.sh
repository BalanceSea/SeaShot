#!/usr/bin/env bash
# 打开/关闭 portal 的调试日志（排查 restore token 用；可逆）
#
#   bash scripts/portal-debug.sh on    # 开后重启 portal 服务
#   bash scripts/portal-debug.sh off   # 关掉
#
# 打开后会看到前端（xdg-desktop-portal）的 g_debug，其中这两行是判定关键：
#   "Replacing restore data received from portal impl with a token"  → 后端给了 restore_data（=勾选了）
#   "Replacing 'restore_token' with portal-specific data"           → 客户端带了 token（走静默恢复）
# 以及权限库相关的警告：
#   "Error setting permission store value: ..."

set -u
ACTION="${1:-on}"

if [ "$ACTION" = "on" ]; then
    systemctl --user set-environment G_MESSAGES_DEBUG=all
    systemctl --user set-environment QT_LOGGING_RULES='xdg_desktop_portal_kde.debug=true'
    echo "已设置调试环境变量，重启 portal 服务…"
else
    systemctl --user unset-environment G_MESSAGES_DEBUG QT_LOGGING_RULES
    echo "已清除调试环境变量，重启 portal 服务…"
fi

systemctl --user restart xdg-desktop-portal.service 2>/dev/null
systemctl --user restart plasma-xdg-desktop-portal-kde.service 2>/dev/null
sleep 1

echo
for unit in xdg-desktop-portal.service plasma-xdg-desktop-portal-kde.service; do
    printf '  %-42s %s\n' "$unit" "$(systemctl --user is-active $unit 2>/dev/null)"
done
echo
if [ "$ACTION" = "on" ]; then
    echo "现在可以跑： bash scripts/probe-n1-permission-store.sh"
    echo "（对话框里记得勾上 Allow restoring on future sessions）"
else
    echo "调试已关闭（日志恢复安静）"
fi
