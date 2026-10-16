#!/usr/bin/env bash
# SeaShot 安装脚本（**只用用户级目录，不需要 sudo**）
#
# Phase 0 实测：KWin 用「调用方 PID → /proc/<pid>/exe → desktop 文件 →
# X-KDE-DBUS-Restricted-Interfaces」判定截图授权。所以：
#   1) 二进制必须装到固定路径（默认 ~/.local/bin/seashot）
#   2) desktop 文件的 Exec 必须与它**完全一致**
#   3) 写完要刷新 KService 缓存（kbuildsycoca6）
set -euo pipefail

ROOT="$(cd "$(dirname "${BASH_SOURCE[0]}")" && pwd)"
BUILD_DIR="${BUILD_DIR:-$ROOT/build}"
BINDIR="${BINDIR:-$HOME/.local/bin}"
APPDIR="${APPDIR:-$HOME/.local/share/applications}"
BIN="$BINDIR/seashot"

DO_BUILD=1
DO_AUTOSTART=0
for arg in "$@"; do
    case "$arg" in
        --no-build) DO_BUILD=0 ;;
        --autostart) DO_AUTOSTART=1 ;;
        -h|--help)
            echo "用法: $0 [--no-build] [--autostart]"
            exit 0
            ;;
    esac
done

if [ "$DO_BUILD" = 1 ]; then
    echo "== 构建 =="
    cmake -S "$ROOT" -B "$BUILD_DIR" -G Ninja -DCMAKE_BUILD_TYPE=Release
    cmake --build "$BUILD_DIR"
fi

echo "== 安装二进制 =="
SEASHOT_BINARY="$BUILD_DIR/seashot"
[ -x "$SEASHOT_BINARY" ] || SEASHOT_BINARY="$BUILD_DIR/src/seashot"
if [ ! -x "$SEASHOT_BINARY" ]; then
    echo "找不到构建产物（试过 $BUILD_DIR/seashot 与 $BUILD_DIR/src/seashot）" >&2
    exit 1
fi
install -Dm755 "$SEASHOT_BINARY" "$BIN"
echo "   $BIN"

echo "== 写 desktop 文件（含截图授权属性）=="
mkdir -p "$APPDIR"
cat > "$APPDIR/seashot.desktop" <<EOF
[Desktop Entry]
Type=Application
Name=SeaShot
GenericName=截图工具
Comment=KDE Plasma 6 (Wayland/X11) 截图工具
Exec=$BIN
TryExec=$BIN
Icon=camera-photo
Terminal=false
NoDisplay=false
Categories=Graphics;Utility;Qt;KDE;
X-KDE-DBUS-Restricted-Interfaces=org.kde.KWin.ScreenShot2
EOF
grep -E '^(Exec|X-KDE)' "$APPDIR/seashot.desktop" | sed 's/^/   /'

if command -v kbuildsycoca6 >/dev/null 2>&1; then
    echo "== 刷新 KService 缓存（KWin 找 desktop 文件靠它）=="
    kbuildsycoca6 --noincremental >/dev/null 2>&1 || true
fi

if [ "$DO_AUTOSTART" = 1 ]; then
    echo "== 安装开机自启 =="
    install -Dm644 "$APPDIR/seashot.desktop" "$HOME/.config/autostart/seashot.desktop"
    echo "   ~/.config/autostart/seashot.desktop"
fi

# PATH 检查：很多发行版（包括本机）默认 PATH 里没有 ~/.local/bin
case ":$PATH:" in
    *":$BINDIR:"*) IN_PATH=1 ;;
    *) IN_PATH=0 ;;
 esac

cat <<EOF

完成。下一步：
  $BIN --selftest          # 逐项自检（授权/脚本/剪贴板/截图）
  $BIN                     # 常驻（托盘 + 等待快捷键）
  $BIN --capture           # 立刻开始一次交互式截图（可绑到 KDE 自定义快捷键）
  系统设置 → 快捷键 → 添加命令：$BIN --capture
EOF

if [ "$IN_PATH" = 0 ]; then
    cat <<EOF

注意：$BINDIR 不在你的 PATH 里，所以直接敲 seashot 会报「未知的命令」。三选一：
  1) 用完整路径： $BIN --capture
  2) 加进 PATH（fish）： fish_add_path $BINDIR      # 立即生效并永久保存
     加进 PATH（bash/zsh）： echo 'export PATH="\$HOME/.local/bin:\$PATH"' >> ~/.profile
  3) 不改 PATH：在 KRunner / 应用菜单里搜 “SeaShot”（desktop 文件已装好）
EOF
fi
