#!/usr/bin/env bash
# SeaShot 安装脚本：系统依赖按需使用 sudo，程序安装到当前用户目录。
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
ICONDIR="${ICONDIR:-$HOME/.local/share/icons/hicolor}"
BIN="$BINDIR/seashot"
APPICON="$ICONDIR/512x512/apps/seashot.png"

DO_BUILD=1
DO_AUTOSTART=0
DO_DEPS=1
DEPS_ONLY=0
ASSUME_YES=0
for arg in "$@"; do
    case "$arg" in
        --no-build) DO_BUILD=0 ;;
        --autostart) DO_AUTOSTART=1 ;;
        --skip-deps) DO_DEPS=0 ;;
        --deps-only) DEPS_ONLY=1 ;;
        -y|--yes) ASSUME_YES=1 ;;
        -h|--help)
            cat <<EOF
用法: $0 [选项]
  默认操作        安装依赖、构建并安装 SeaShot
  --skip-deps     跳过系统依赖安装（已准备好依赖时使用）
  --deps-only     仅安装依赖，不构建或安装程序
  --no-build      跳过构建，安装已有的构建产物
  --autostart     添加当前用户的登录自启
  -y, --yes       自动确认包管理器的安装提示
  -h, --help      显示帮助

请直接运行脚本；只有安装系统依赖时会调用 sudo。
EOF
            exit 0
            ;;
        *) echo "未知参数：$arg（使用 --help 查看用法）" >&2; exit 1 ;;
    esac
done

die() { echo "错误：$*" >&2; exit 1; }

if [ "$DO_DEPS" = 0 ] && [ "$DEPS_ONLY" = 1 ]; then
    die "--skip-deps 与 --deps-only 不能同时使用。"
fi
if [ "$EUID" = 0 ] && [ -n "${SUDO_USER:-}" ] && [ "$DEPS_ONLY" = 0 ]; then
    die "请不要使用 sudo 运行整个脚本；直接运行 ./install.sh，避免将程序安装到 root 的用户目录。"
fi

run_as_root() {
    if [ "$EUID" = 0 ]; then
        "$@"
    elif command -v sudo >/dev/null 2>&1; then
        sudo -- "$@"
    else
        die "安装系统依赖需要 sudo；请让管理员安装依赖，再使用 --skip-deps。"
    fi
}

install_dependencies() {
    local ID= ID_LIKE= PRETTY_NAME= distro= candidate
    local -a packages confirm=()
    if [ -r /etc/os-release ]; then
        # shellcheck source=/dev/null
        . /etc/os-release
    elif [ -r /usr/lib/os-release ]; then
        # shellcheck source=/dev/null
        . /usr/lib/os-release
    else
        die "找不到 os-release，无法识别发行版；请手动安装依赖后使用 --skip-deps。"
    fi
    # 优先匹配发行版本身，再使用它声明的上游发行版。
    for candidate in $ID $ID_LIKE; do
        case "$candidate" in
            fedora|arch|debian|ubuntu) distro="$candidate"; break ;;
        esac
    done
    echo "== 安装系统依赖：${PRETTY_NAME:-${ID:-未知发行版}} =="
    case "$distro" in
        fedora)
            if [ -e /run/ostree-booted ]; then
                die "当前为不可变 Fedora 系统；请先用系统对应的方式准备依赖，再使用 --skip-deps。"
            fi
            local dnf=dnf
            command -v dnf >/dev/null 2>&1 || dnf=dnf5
            command -v "$dnf" >/dev/null 2>&1 || die "找不到 dnf/dnf5。"
            packages=(
                gcc-c++ cmake ninja-build pkgconf-pkg-config extra-cmake-modules
                qt6-qtbase-devel qt6-qtwayland qt6-qtwayland-devel
                kf6-kglobalaccel-devel kf6-kstatusnotifieritem-devel layer-shell-qt-devel
                pipewire-devel libxcb-devel xcb-util-wm-devel
                kf6-kservice xdg-desktop-portal-kde wl-clipboard
                tesseract tesseract-langpack-eng tesseract-langpack-chi_sim
            )
            # 已有的完整版 FFmpeg 不与 Fedora 的 ffmpeg-free 相互替换。
            command -v ffmpeg >/dev/null 2>&1 || packages+=(ffmpeg-free)
            [ "$ASSUME_YES" = 0 ] || confirm=(-y)
            run_as_root "$dnf" install "${confirm[@]}" "${packages[@]}" || return 1
            ;;
        arch)
            command -v pacman >/dev/null 2>&1 || die "找不到 pacman。"
            packages=(
                gcc make cmake ninja pkgconf extra-cmake-modules
                qt6-base qt6-wayland kglobalaccel kstatusnotifieritem layer-shell-qt
                libpipewire libxcb xcb-util-wm kservice xdg-desktop-portal-kde
                wl-clipboard ffmpeg tesseract tesseract-data-eng tesseract-data-chi_sim
            )
            [ "$ASSUME_YES" = 0 ] || confirm=(--noconfirm)
            run_as_root pacman -S --needed "${confirm[@]}" "${packages[@]}" || return 1
            ;;
        debian|ubuntu)
            command -v apt-get >/dev/null 2>&1 || die "找不到 apt-get。"
            packages=(
                build-essential cmake ninja-build pkg-config extra-cmake-modules
                qt6-base-dev qt6-wayland qt6-wayland-dev
                libkf6globalaccel-dev libkf6statusnotifieritem-dev
                liblayershellqtinterface-dev libpipewire-0.3-dev
                libxcb1-dev libxcb-shm0-dev libxcb-ewmh-dev
                libkf6service-bin xdg-desktop-portal-kde wl-clipboard ffmpeg
                tesseract-ocr tesseract-ocr-eng tesseract-ocr-chi-sim
            )
            [ "$ASSUME_YES" = 0 ] || confirm=(-y)
            run_as_root apt-get update || return 1
            run_as_root apt-get install "${confirm[@]}" "${packages[@]}" || return 1
            ;;
        *) die "暂不支持自动安装此发行版的依赖；请按 README 准备 Qt ≥ 6.5、KF6 和 LayerShellQt 6 等依赖，再使用 --skip-deps。" ;;
    esac
    # Fedora 官方的 FFmpeg 可能不提供项目所需的 H.264 编码器。
    local encoders
    if encoders="$(ffmpeg -hide_banner -encoders 2>/dev/null)"; then
        if [[ ! "$encoders" =~ [[:space:]]libx264[[:space:]] ]]; then
            echo "提示：当前 FFmpeg 没有 libx264，MP4 导出和 H.264 录制不可用；GIF 与默认 FFV1 录制仍可使用。" >&2
            echo "      如需 H.264，请自行准备包含 libx264 的 FFmpeg 后重新运行；脚本不会添加第三方软件源。" >&2
        fi
    fi
}

if [ "$DO_DEPS" = 1 ]; then
    install_dependencies || die "依赖安装失败，已停止后续操作。请检查网络、软件源及 Qt/KF6 版本；已手动准备依赖时可使用 --skip-deps。"
fi
if [ "$DEPS_ONLY" = 1 ]; then
    echo "依赖安装完成。"
    exit 0
fi

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
if ! cmp -s "$SEASHOT_BINARY" "$BIN"; then
    install -Dm755 "$SEASHOT_BINARY" "$BIN"
fi
echo "   $BIN"

echo "== 安装 SeaShot 应用图标 =="
install -Dm644 "$ROOT/resources/icons/seashot.png" "$APPICON"
if command -v gtk-update-icon-cache >/dev/null 2>&1; then
    gtk-update-icon-cache -f -t "$ICONDIR" >/dev/null 2>&1 || true
fi

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
Icon=$APPICON
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
