# SeaShot

KDE Plasma 6（Wayland / X11 双后端）下的截图工具：自动框选窗口（悬停出现虚线框，左键单击即截）、
自由区域选取、编辑（Phase 2）、GIF 录制（Phase 4）、复制到剪贴板、保存到指定位置、可绑定的全局快捷键。

C++20 + Qt 6 + CMake。Phase 0 的能力探测结论见 [`docs/phase0-findings.md`](docs/phase0-findings.md)，
实施计划见 [`PLAN.md`](PLAN.md)。

## 构建

```bash
cmake -S . -B build -G Ninja -DCMAKE_BUILD_TYPE=Release
cmake --build build
ctest --test-dir build --output-on-failure
```

依赖：Qt6（Core/Gui/Widgets/DBus/Network/Test）、KF6 GlobalAccel、KF6 StatusNotifierItem、
LayerShellQt、xcb + xcb-ewmh（缺失时自动关闭 X11 后端）、`wl-copy`（wl-clipboard，用于剪贴板持久化）。

## 安装（不需要 sudo）

```bash
./install.sh              # 构建 + 装到 ~/.local/bin + 写 desktop 文件 + 刷新 sycoca
./install.sh --autostart  # 额外装开机自启
```

**为什么必须跑 install.sh**：KWin 判定截图授权的方式是
「调用方 PID → `/proc/<pid>/exe` → desktop 文件 → `X-KDE-DBUS-Restricted-Interfaces`」。
没装 desktop 文件（或 `Exec` 与实际运行的二进制不一致）时，
`org.kde.KWin.ScreenShot2` 会直接返回 `Error.NoAuthorized`。详见 Phase 0 findings。

## 用法

```bash
seashot                     # 常驻：托盘 +（若已绑定）全局快捷键
seashot --capture           # 开始一次交互式截图；已有实例时转发给它
seashot --selftest          # 逐项自检：授权 / KWin 脚本 / 窗口枚举 / 截图 / 剪贴板
seashot --list-windows      # 打印窗口列表（handle、几何、栈序、自排除标志）

# 脚本化（免交互，便于验收）
seashot --capture-screen --save /tmp/full.png
seashot --rect 100,200,400,300 --save /tmp/rect.png [--no-copy]
seashot --window '{uuid}' --save /tmp/window.png
seashot --overlay-hold 5000          # 显示选择覆盖层 5 秒后退出（调试/验收）
seashot --open-editor                # 本次截图后直接打开编辑器（覆盖配置）
seashot --settings                   # 打开配置界面（托盘菜单「设置…」同此）
seashot --print-config               # 打印生效中的配置（脚本/排查用）
seashot --set-hotkey "Ctrl+Alt+S"     # 写入并注册全局快捷键（none = 解绑）
seashot --hotkey-status              # 打印当前实际生效的快捷键
seashot --backend=x11 --list-windows # 显式选后端（XWayland 冒烟用）
```

输出格式（便于脚本判定）：`CAPTURE ok size=WxH scale=S` / `SAVED <path>` / `COPIED image/png` /
`CAPTURE_ERROR <原因>` / `COPY_ERROR <原因>`。

## 快捷键

三种方式（互不冲突）：

- **SeaShot 配置界面**（`seashot --settings` 或托盘菜单「设置…」）：直接按录快捷键，
  立即写进 KGlobalAccel 并落盘到 `shortcut/hotkey`；
- **KDE 系统设置**：系统设置 → 快捷键 → SeaShot → `capture`；
- **自定义命令**：把 `seashot --capture` 绑成 KDE 自定义快捷键。

PLAN 已定：**默认不注册任何键**（实测 `Ctrl+Alt+A` 已被本机 Spectacle 启动器占用）。

## 交互约定

- 悬停窗口：虚线框贴 `frameGeometry`（含标题栏的窗口实体边界）
- 左键单击：截取该窗口（Wayland 走 `CaptureWindow(include-decoration=true)`，即所见即所得：
  含标题栏/边框/圆角/阴影；默认保留阴影边距，`capture/keepShadowMargin=false` 可裁到窗口实体）
- 按住拖动：按矩形选区截图
- `Esc` / 右键：取消
- 覆盖层与结果浮层通过 KWin 的 `excludeFromCapture` 自我排除，不会出现在任何截图里

### 截图后的结果浮层（按你的要求定稿）

- **按原尺寸显示**：截图多大卡片就多大（1:1）；整张图在屏幕可用区放不下时才等比缩小，
  并在文字里写明「已缩到 W×H 显示」
- **贴着截图框的左下角**：卡片左上角 = 选区（或窗口）的左下角；下方放不下就改成卡片左下角
  与选区左下角对齐（往上盖）
- **外观跟随系统主题**（用 `QPalette` 取色，Breeze 亮/暗主题都跟；当前在你机器上是 BreezeLight）
- 底部一条浮在图片上的按钮条：「编辑」（主按钮，高亮） / 另存为 / 复制 / 关闭；
  左侧写明「已截图 W×H」（若已复制/已保存也一并显示）
- 按住图片区域可以**拖动浮层**；拖动时坐标按 8ms 合并提交、卡片内容缓存成 `QPixmap`
  （layer-shell 下每次 setMargins 都是一次 commit，逐事件提交会明显卡顿）
- **描边**：外圈 3px 深色 + 内圈 2px 主题高亮色，浅色/深色桌面下都能一眼找到截图
- **不自动消失**（默认 0）：只在用户操作后消失——`Enter`/`Esc`、另存为完成、点「编辑」、
  点「关闭」；若想要兜底超时，设 `ui/toastAutoHideMs`（毫秒）
- 「另存为」**弹文件对话框**（不再默默落到默认目录）；「复制」只更新状态文字、不关浮层
- 浮层用 layer-shell（`LayerOverlay`）实现，所以**全屏应用前面也能看见**
- 带 `--save` 的脚本化调用（验收/自动化）不弹浮层，交付完就退出

## 录制 GIF

```fish
seashot --record                          # 交互选区域（拖动选区，或单击窗口录其外框）
seashot --record --rect 300,200,320,240   # 直接录指定矩形
seashot --record --seconds 10             # 录 10 秒后自动停
seashot --record-stop                     # 让常驻实例停止录制
```

- 托盘菜单「录制 GIF…」（录制中会变成「停止录制」）；录制中按全局快捷键 = 停止
- 录制时区域左上方出现指示器：闪烁红点 + 计时 + 已录帧数（丢帧会显示）+「停止」按钮
  （放区域**外面**，所以不会被录进 GIF）
- 帧率用设置里的 `record/fps`（默认 15）；**不设时长与体积上限**，帧直接流进 ffmpeg
- 编码是两遍：FFV1 无损中间文件 → `palettegen`/`paletteuse` 出调色板优化的 GIF，
  中间文件收尾后自动删除
- 采帧方式：定时调用截图后端（Wayland=ScreenShot2 / X11=MIT-SHM）。实测 15fps 时
  有效帧率 15.19、丢帧 0；如需更高帧率/更大区域，见 `docs/known-issues.md` ISSUE-006

## 配置界面与托盘

```fish
seashot --settings     # 或常驻后点托盘图标 → 「设置…」
```

- 配置界面分五组：截图后 / 保存 / 界面 / 全局快捷键 / GIF 录制（Phase 4 生效）
- **改动即时生效**（不用点保存），落盘到 `~/.config/seashot/seashot.conf`；「恢复默认」= 清空配置
- 托盘菜单：截图 / 设置… / KDE 快捷键设置… / 退出；按 `ui/trayPersistent` 决定是否常驻
- 托盘图标需要桌面提供 `StatusNotifierHost`（本机已确认；没有 host 的桌面只会记日志不会崩）

## 配置

`~/.config/seashot/seashot.conf`（改动即时生效）：

| 键 | 默认 | 说明 |
| --- | --- | --- |
| `capture/copyAfterCapture` | `true` | 截图后写入剪贴板 |
| `capture/saveDir` | `~/Pictures/Screenshots` | 默认保存目录 |
| `capture/filenameTemplate` | `seashot_%1` | `%1`=时间戳，另有 `%d`、`%t`；同名自动加序号 |
| `capture/autoSaveAfterCapture` | `false` | 不点按钮也自动落盘 |
| `capture/openEditorAfterCapture` | `false` | 截图后直接打开编辑器（不弹浮层） |
| `capture/keepShadowMargin` | `true` | 窗口图保留阴影边距 |
| `shortcut/hotkey` | 空 | 全局快捷键（空=不绑定） |
| `record/fps` | `15` | GIF 帧率（Phase 4） |
| `ui/trayPersistent` | `true` | 常驻托盘 |
| `ui/toastAutoHideMs` | `0` | 结果浮层兜底自动消失毫秒（`0` = 不自动消失，按你的要求） |

## 调试

- `SEASHOT_DISABLE_SELF_EXCLUSION=1 seashot …`：本次不把 SeaShot 自己的窗口排除出截图。
  调试浮层/覆盖层外观时用（默认它们不会被任何截图拍到）。
- 日志走 stderr（`[app]`/`[kwin]`/`[shot2]`/`[toast]` 前缀）；Qt 默认会吞掉 `qInfo`，
  所以 SeaShot 显式写 stderr。
- KWin 助手脚本残留在 `~/.cache/seashot/seashot/*.js`；若上次是被 kill 的，
  下次启动会自动卸载并删除陈旧脚本（否则它会把之后所有 SeaShot 窗口标成排除出截图）。

## 已知限制（与实测依据）

- **X11 会话未实测**：本机没有可登录的 X11 会话（`/usr/share/xsessions` 为空），X11 后端只有
  单元测试 + XWayland 冒烟。XWayland 下只能看到/截取 X11 客户端窗口；X11 下窗口被遮挡时
  会截到遮挡物（无 `excludeFromCapture` 等价物，因此截图前会先隐藏覆盖层并等一帧）。
- **分数缩放与多输出未实测**：本机是单输出、`scale=1`。转换层有合成单测覆盖，
  真机验证需要临时改显示缩放。
- **GIF 录制（Phase 4）**：KWin 6.7 没有录制 D-Bus 接口（Phase 0 已确认），
  计划走 portal `ScreenCast` + `ffmpeg` 编码；首次会弹授权对话框（尚未验证）。
- 覆盖层按「虚拟桌面并集」做成一个窗口；多输出下跨屏拖选需要进一步处理。

## 目录结构

```
src/backend/{wayland,x11}/   后端：KWin 脚本桥 + ScreenShot2 / EWMH + XGetImage
src/core/                    公共类型 + 选区纯逻辑
src/geometry/                逻辑↔物理坐标的唯一转换点（ScreenMapper）
src/overlay/                 选择模式覆盖层（layer-shell + QPainter）
src/app/                     应用编排、配置、托盘、结果浮层、自检
resources/seashot-kwin.js    KWin 助手脚本（窗口快照 + 自排除），随二进制内嵌
tests/                       QtTest 单元测试（坐标/选区/EWMH/配置）
probes/                      Phase 0 的可重复探测脚本与原始证据
docs/                        Phase 0 结论与各阶段验收记录
```
