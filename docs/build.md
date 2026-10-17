# 🔨 构建与安装

本文是 `README.md` 安装一节的展开：依赖、各发行版说明、`install.sh` 全部开关、手动编译与安装，
以及 **Wayland 下为什么必须用安装脚本装**。

## 一键安装（推荐）

```bash
./install.sh                 # 装依赖 → 构建 → 安装到当前用户
./install.sh --autostart     # 同上，并添加登录自启
./install.sh --skip-deps     # 依赖已就绪：只构建 + 安装
./install.sh --deps-only     # 只装依赖
./install.sh --no-build      # 不构建，安装已有产物
```

- 脚本**只用用户级目录**（`~/.local`），但**装系统依赖时需要 sudo**；**不要用 sudo 运行整个脚本**。
- 支持自动装依赖的发行版：Fedora、Arch Linux、Debian / Ubuntu 及声明兼容这些发行版的衍生系统
  （通过 `os-release` 识别，用对应包管理器）。
- 软件源需提供 **Qt ≥ 6.5、KF6、LayerShellQt 6**；旧发行版缺包时请先升级系统，或自行准备依赖后
  用 `--skip-deps`。
- 不可变 Fedora（Silverblue/Kinoite）请先用系统对应方式准备依赖，再用 `--skip-deps`。
- Fedora 默认装 `ffmpeg-free`（已有 `ffmpeg` 时保留）；若检测到缺 `libx264` 会提示
  **MP4 / H.264 不可用**，GIF 与默认的 FFV1 录制仍然可用。

| 开关 | 作用 |
| --- | --- |
| `--skip-deps` | 跳过系统依赖安装（依赖已准备好时用） |
| `--deps-only` | 仅安装依赖，不构建也不安装程序（与 `--skip-deps` 互斥） |
| `--no-build` | 跳过构建，安装 `build/` 里已有的产物 |
| `--autostart` | 添加当前用户的登录自启（写 `~/.config/autostart/seashot.desktop`） |
| `-y` / `--yes` | 自动确认包管理器的安装提示 |
| `-h` / `--help` | 显示用法 |

## 依赖

手动准备依赖或使用其他发行版时，按需安装后传 `--skip-deps`：

| 类型 | 依赖 |
| --- | --- |
| 🏗️ 构建工具 | 支持 C++20 的编译器、CMake ≥ 3.21、Ninja、pkg-config |
| 🪟 界面与桌面 | Qt ≥ 6.5：Core、Gui、Widgets、DBus、Network、Test；KF6 GlobalAccel、KF6 StatusNotifierItem；LayerShellQt |
| 🔌 X11 后端（可选） | `xcb`、`xcb-shm`、`xcb-ewmh`；缺失时自动关闭 X11 后端 |
| 📡 PipeWire 帧源（可选） | `libpipewire-0.3`；缺失或不可用时回退到截图轮询录制 |
| 🎞️ 录制与导出 | `ffmpeg`，含 FFV1、libx264 与 GIF 调色板滤镜；部分测试还需要 `ffprobe` |
| 📋 Wayland 剪贴板 | 推荐 `wl-clipboard`（`wl-copy` 负责让复制内容在进程退出后仍然可用） |
| 🔤 OCR（可选） | `tesseract` 及所需语言包，例如 `chi_sim`（中文简体）、`eng`（英文） |

## 手动编译与安装

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build -j 2

# 依赖已就绪，只安装刚才构建的版本（无需 sudo）
./install.sh --skip-deps --no-build
```

安装结果：

- 二进制：`~/.local/bin/seashot`
- 应用菜单入口：`~/.local/share/applications/seashot.desktop`
- 图标：`resources/icons/seashot.png` → 用户图标目录
- 登录自启（仅 `--autostart`）：`~/.config/autostart/seashot.desktop`

## ⚠️ Wayland 下必须用安装脚本装

KWin 判定"谁可以调用 `org.kde.KWin.ScreenShot2`"的依据是：

```
调用方 PID → /proc/<pid>/exe → 匹配到的 desktop 文件 → X-KDE-DBUS-Restricted-Interfaces
```

所以：

1. 二进制必须装在**固定路径**（默认 `~/.local/bin/seashot`）；
2. desktop 文件里的 `Exec=` 必须与该路径**逐字一致**；
3. 脚本写完还会刷新 KService 缓存（`kbuildsycoca6 --noincremental`），KWin 才能找到它。

**直接从 `build/` 目录运行可能报 `Error.NoAuthorized`。** 装好后如果菜单/授权仍不生效，
注销重登一次即可（KService 缓存刷新后通常立刻生效）。

## 找不到 `seashot` 命令？

多数发行版的 `PATH` 默认不含 `~/.local/bin`。三种处理：

```bash
~/.local/bin/seashot --capture                       # 1) 用完整路径
export PATH="$HOME/.local/bin:$PATH"                 # 2) 加进 shell 配置（当前会话生效）
echo 'export PATH="$HOME/.local/bin:$PATH"' >> ~/.profile   # 3) 写进登录环境
```

KDE 的快捷键/自启配置里建议直接写**完整路径**，避免继承到不含 `~/.local/bin` 的 `PATH`。
