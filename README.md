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
seashot --inplace-demo /tmp/a1       # 就地标注（A1/A2/A3）自动验收：拖框→1:1/手柄/工具条/标注判定→落盘 /tmp/a1-*.png
SEASHOT_INPLACE_DEMO_COMMIT=1 seashot --inplace-demo /tmp/a1 --save /tmp/out.png --no-copy
                                     # 同上，但最后合成 Enter（完成），验“标注真的进结果图”
SEASHOT_INPLACE_DEMO_ACTION=copy|save|pin seashot --inplace-demo /tmp/a1 [--save /tmp/out.png]
                                     # 同上，但最后按 Ctrl+C / Ctrl+S / Ctrl+T（验三个交付动作）
seashot --pin-demo /tmp/p              # 贴图（Phase C）自动验收：拖动/缩放/菜单/多张并存/关闭
                                       # 加 SEASHOT_PIN_DEMO_HOLD=<ms> 可延长保持时间（配合外部截屏）
seashot --open-editor                # 本次截图后直接打开编辑器（覆盖配置）
seashot --scroll-capture --rect 100,100,800,600 --save /tmp/long.png
                                     # 滚动截屏（Phase D）：自己滚页面，SeaShot 定时抓帧拼成长图
                                     # （--scroll-ms N 限制最长运行时间，默认 20000）
seashot --ocr /tmp/x.png             # OCR 文字识别（Phase E）：结果打到 stdout（含 OCR_TEXT_BEGIN/END 标记）
                                     # --ocr-langs chi_sim+eng 可指定语言（默认自动）
seashot --pin /tmp/x.png             # 把一张图贴（钉）到屏幕上（Phase C；拖动/滚轮缩放/右键菜单）
seashot --settings                   # 打开配置界面（托盘菜单「设置…」同此）
seashot --print-config               # 打印生效中的配置（脚本/排查用）
seashot --set-hotkey "Ctrl+Alt+S"     # 写入并注册全局快捷键（none = 解绑）
seashot --hotkey-status              # 打印当前实际生效的快捷键
seashot --backend=x11 --list-windows # 显式选后端（XWayland 冒烟用）
```

输出格式（便于脚本判定）：`CAPTURE ok size=WxH scale=S` / `SAVED <path>` / `COPIED image/png` /
`CAPTURE_ERROR <原因>` / `COPY_ERROR <原因>`。

## 快捷键

**推荐：让 SeaShot 自己向系统申请注册（Wayland/门户）** —— 点「设置 → 全局快捷键」里的
**「向系统申请注册（会弹授权对话框）」**（或托盘菜单「注册全局快捷键（由我申请）…」，
或命令行 `seashot --hotkey-portal-request`）。系统会弹一个**授权对话框**列出请求的快捷键
（预填一个键、可当场改），**你点允许它才生效**；拒绝/关掉则不注册。
- **首次运行会自动问一次**：常驻实例第一次启动时（配置里没有键、门户里也没有绑定），
  会自动发起注册 → 于是系统弹一次授权对话框，由你决定；之后**不再打扰**
  （想再问就点设置里的按钮）。验收/自动化用 `SEASHOT_NO_FIRST_RUN_ASK=1` 关掉。
- 想改键：同一页的「更改快捷键（系统对话框）…」，或 `--hotkey-portal-configure`
- 看状态：`seashot --hotkey-portal-status`；`--hotkey-status` 会同时报"门户这条路"和
  "KGlobalAccel 那条路"的状态
- 说明：KDE 6 里 KGlobalAccel 与门户**共用同一份快捷键存储**，所以已经被 KGlobalAccel 建过条目的
  id 不会再弹授权（想重新体验首次授权：停 `plasma-kglobalaccel.service`、从
  `~/.config/kglobalshortcutsrc` 的 `[seashot]` 段删掉那个 id，再发起）

三种方式（互不冲突）：

- **SeaShot 配置界面**（`seashot --settings` 或托盘菜单「设置…」）：直接按录快捷键，
  立即写进 KGlobalAccel 并落盘到 `shortcut/hotkey`；
- **KDE 系统设置**：系统设置 → 快捷键 → SeaShot → `capture`；
- **自定义命令**：把 `seashot --capture` 绑成 KDE 自定义快捷键。

PLAN 已定：**默认不注册任何键**（实测 `Ctrl+Alt+A` 已被本机 Spectacle 启动器占用）。

## 交互约定

- **截图后不弹窗**（2026-10-17 用户要求）：截图完成直接**复制到剪贴板**（设置里"自动复制"默认开），
  按设置也会自动保存；屏幕上不出现任何浮层。要看图 / 另存 / 编辑：**托盘菜单
  「保存 / 导出上一次结果…」**。
- **「保存 / 导出」窗口**把两件事收在一处：**截图**页（复制 / 保存到默认目录 / 另存为… /
  在编辑器里打开）与**录制**页（导出 GIF / 导出 MP4 / 复制 GIF）。
  录制结束后会**自动打开**这个窗口的录制页，由你决定导出格式。
- 悬停窗口：虚线框贴 `frameGeometry`（含标题栏的窗口实体边界）
- **就地标注（Phase A1/A2）**：交互式截图（热键 / 托盘 / `--capture`）拖框选完区域后
  **不打开新窗口**，而是在同一个 layer-shell 覆盖层上就地显示冻结屏幕（**逻辑像素 1:1**，
  选区外压暗），选区带边框与 8 个手柄；紧贴选区下方是一条**自绘紧凑图标工具条**
  （下方放不下则自动跑到上方）：10 个工具 + 选区尺寸 + **✓ 完成** / **✕ 取消**。
  - 工具切换：鼠标点，或数字键 `1`–`9`（选择/矩形/椭圆/箭头/画笔/高亮/文字/序号/马赛克）；
    「模糊」在最后，点击可达、暂无数字键；鼠标悬停会显示工具名与数字键
  - **直接画**：形状工具拖一下；画笔/高亮按轨迹；序号单击放置；文字点一下后用键盘输入（回车确认、Esc 丢弃）
  - 选择工具（`1`）：点选标注、拖整体移动、拖角点改尺寸、双击文字改字
  - **选区随时可改**：拖 8 个手柄或 4 条边改尺寸、在选区内部（没点到标注时）拖动整个选区，
    越界会自动夹到屏幕内、最小 16×16；鼠标形状会变成对应的缩放/移动光标。
    **标注锚定在画面内容上**：把框拉大/挪动，标注仍压在同一块画面上（不会跟着框跑）
  - **颜色/粗细**：工具条上的色块与短横线各是一个自绘弹出面板 —— 色板（10 个预设色）与滑杆（1–24 px）；
    选完对**之后新画**的标注生效（面板内空白点击不误画，`Esc` 先关面板）
  - `Ctrl+Z` / `Ctrl+Shift+Z` / `Ctrl+Y` 撤销重做；`Delete` 删除选中；
    **方向键**微调选区位置 1px（`Shift` = 10px）；**`Ctrl+A`** 全选整屏
  - **`Ctrl+C`** = 完成并复制（不弹浮层，与 QQ 一致）；**`Ctrl+S`** = 完成并保存到默认目录；
    **`Ctrl+T`** = 完成并**贴图**（把结果钉到屏幕上，工具条上也有 📌 格）；
    **`Ctrl+L`** = 完成并**长截图**（滚动截屏，工具条上也有对应格）；
    **`Ctrl+O`** = 对选区做**文字识别**（OCR，工具条上也有「OCR」格）
  - `Enter` / 双击 / ✓ = 完成（**标注会合成进结果**，之后按设置复制/保存/弹浮层）；
    `Esc` / 右键 / ✕ = 取消（`Esc` 逐级：关面板 → 中断选区拖动 → 丢文字草稿 → 取消选中 → 退出）
  - 颜色/粗细暂时**不持久化**（一次会话内有效，默认 `#ff4040` / 4px）
  - 脚本式调用（`--rect` / `--window` / `--save` / `--capture-screen`）**不走这条路**，行为与以前一致。
    临时退回旧行为：`SEASHOT_INPLACE_ANNOTATE=0`。
- **OCR 文字识别（Phase E）**：选完区域后点工具条「OCR」（或 `Ctrl+O`）→ 识别选区里的文字
  （识别"屏幕上看到的那张图"，含你的标注）→ 弹出结果面板：可**复制全部**、**复制选中**
  （没选中时复制光标所在行）、`Esc` 关闭。命令行 `seashot --ocr x.png` 可直接把文本打到 stdout。
  依赖系统 `tesseract` + 语言包（缺了会明确告诉你装什么）。
- **长截图 / 滚动截屏（Phase D）**：选完区域后点工具条「长截图」（或 `Ctrl+L`），然后
  **自己滚页面**（滚轮/触控板/拖滚动条都行，SeaShot 不注入滚动事件）——
  它每 250ms 抓一帧、按重叠区对齐拼接；**不会**因为"没检测到滚动"就自己收工 ——
  **滚完后由你点指示器上的「完成」或按 `Esc` 结束**（兜底：高度 20000px / 30 分钟）。
  长图会弹进结果浮层（可复制/保存/贴图/编辑）。自动裁掉**每帧都一样的页头/底栏**与
  **每帧都在变的贴边滚动条列**；对不上的一帧（平滑滚动的中间帧）会被拒掉。
  命令行 `seashot --scroll-capture --rect x,y,w,h --save out.png` 可脚本化。
- **贴图 / 钉图（Phase C）**：把一张图钉在屏幕上的一块浮窗里（layer-shell `LayerOverlay` 层，
  会压在全屏应用之上）：**按住拖动**、**滚轮缩放**（0.2×–5×，以光标为锚点）、
  **右键菜单**（复制 / 保存到默认目录 / 关闭）、`Esc` 关闭（先点一下贴图拿焦点）；
  支持多张并存；贴图以外的地方点击**穿透到桌面**，不挡正常工作。
  入口：就地标注里 `Ctrl+T`（或工具条上的 📌），或命令行 `seashot --pin <png>`。
  ⚠️ 贴图和覆盖层/结果浮层一样被"自排除"出截图 —— **肉眼看得到，但新截图里拍不到**
  （详见 `docs/acceptance/phaseC.md`）。
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

`seashot --settings` 打开**左侧导航 + 右侧页面**的配置界面（图标来自项目内嵌资源）。
配置文件：`~/.config/seashot/seashot.conf`（改动即时生效）：

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
- `SEASHOT_INPLACE_ANNOTATE=0 seashot --capture`：本次交互式截图退回
  「选完直接截图」的旧行为（不走就地标注阶段）。
- `SEASHOT_INPLACE_DEMO_COMMIT=1 seashot --inplace-demo <prefix>`：演示最后不发「取消」
  而是合成 `Enter`（完成），用来验收「标注 → 结果图」这条链路。
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
src/overlay/                 选择模式覆盖层（layer-shell + QPainter；含就地标注阶段 Annotating 与自绘工具条）
src/app/                     应用编排、配置、托盘、结果浮层、自检
resources/seashot-kwin.js    KWin 助手脚本（窗口快照 + 自排除），随二进制内嵌
tests/                       QtTest 单元测试（坐标/选区/EWMH/配置）
probes/                      Phase 0 的可重复探测脚本与原始证据
docs/                        Phase 0 结论与各阶段验收记录
```
