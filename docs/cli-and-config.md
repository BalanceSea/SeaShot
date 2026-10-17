# ⌨️ 命令行、配置与快捷键

本文是 `README.md` 用法一节的展开：**全部命令行开关**（以 `src/main.cpp` 为准）、
**配置键全表**（以 `src/app/Settings.cpp` 为准）、选区快捷键，以及便于脚本判定的**输出标记**。

## 交付方式（先看这条）

- **截图完成不弹任何窗口**：结果直接按设置复制到剪贴板（`capture/copyAfterCapture` 默认开），
  按设置也可能自动保存；要看图 / 另存为 / 进编辑器，用**托盘菜单「保存 / 导出上一次结果…」**。
- **录制结束**会自动打开同一个「保存 / 导出」窗口的**录制页**，在窗口里选导出 GIF / MP4 或复制 GIF。
- 脚本式调用（带 `--save`）保持"一次到底"：直接写文件、不弹窗。

## 常用开关

| 开关 | 说明 |
| --- | --- |
| `--capture` | 开始一次交互式截图；已有常驻实例时**转发给它** |
| `--capture-screen` | 免交互：截取整块虚拟桌面并退出 |
| `--rect x,y,w,h` | 免交互：截取矩形（逻辑坐标） |
| `--window <handle>` | 免交互：按窗口 handle 截取（含装饰/阴影，所见即所得） |
| `--save <path>` | 把结果保存到该路径（带此参数即视为脚本式调用） |
| `--no-copy` | 本次不写剪贴板（覆盖设置里的自动复制） |
| `--open-editor` | 本次截图后直接打开编辑器（覆盖 `capture/openEditorAfterCapture`） |
| `--edit <png>` | 用编辑器打开一张图片 |
| `--settings` | 打开配置界面 |
| `--list-windows` | 打印窗口列表（handle、几何、栈序、自排除标志） |
| `--selftest` | 逐项自检：授权 / KWin 脚本 / 窗口枚举 / 截图 / 剪贴板 |
| `--print-config` | 打印生效中的配置（含实际生效的后端与快捷键） |
| `--backend <name>` | `auto`（默认）/ `wayland` / `x11` |
| `--overlay-hold <ms>` | 显示选择覆盖层 N 毫秒后退出（调试/验收用） |

## 录制

| 开关 | 说明 |
| --- | --- |
| `--record` | 交互选区域录制；配合 `--rect` 可直接录指定区域 |
| `--seconds <N>` | 录制 N 秒后自动停止（0 = 手动停） |
| `--record-stop` | 让常驻实例停止正在进行的录制 |
| `--save <path>` | 按扩展名（`.gif` / `.mp4`）直接导出，不弹窗口 |

细节（帧源、帧率、中间文件、丢帧策略）见 [录制与导出](recording.md)。

## 长截图 / OCR / 贴图

| 开关 | 说明 |
| --- | --- |
| `--scroll-capture` | 长截图：自己滚动页面，定时抓帧拼接；配合 `--rect` 可脚本化 |
| `--scroll-ms <ms>` | 长截图最长运行时间（默认 20000；交互式下由用户点「完成」/`Esc` 结束） |
| `--ocr <png>` | 对图片做 OCR，文本打到 stdout（带 `OCR_TEXT_BEGIN` / `OCR_TEXT_END` 标记） |
| `--ocr-langs <langs>` | OCR 语言，如 `chi_sim+eng`（默认自动选已安装的中英） |
| `--pin <png>` | 把图片贴（钉）到屏幕上 |

## 全局快捷键（xdg-desktop-portal GlobalShortcuts）

| 开关 | 说明 |
| --- | --- |
| `--hotkey-portal-request` | 向系统申请注册（**首次/新 id 会弹系统授权对话框**，由你决定允许或拒绝） |
| `--hotkey-portal-status` | 静默报告门户可用性、版本与当前绑定（不弹窗） |
| `--hotkey-portal-configure` | 授权后打开系统的改键对话框 |
| `--hotkey-portal-id <id>` | 快捷键 id（默认 `screenshot`；换新 id 会重新走一次授权） |
| `--hotkey-portal-trigger <keys>` | 期望的按键，如 `CTRL+ALT+S`（XDG 格式，修饰符大写） |
| `--hotkey-portal-ms <ms>` | 等待用户在对话框上应答的上限 |
| `--hotkey-portal-listen-ms <ms>` | 授权成功后继续监听按键多久（0 = 立刻退出） |
| `--set-hotkey <keys>` | 走 KGlobalAccel / X11 路径写入并注册（`none` = 解绑） |
| `--hotkey-status` | 打印 `shortcut/hotkey` 配置值与 KGlobalAccel 实际值，并附门户状态 |

说明：门户已授权时程序**不再注册 KGlobalAccel**（避免系统设置里出现两条同键快捷键）；
快捷键需要应用**常驻**才能响应（推荐 `./install.sh --autostart`）。

## 验收 / 诊断（不参与日常使用）

| 开关 | 说明 |
| --- | --- |
| `--inplace-demo <prefix>` | 就地标注（A1/A2/A3/A3.1/B）自动验收：合成拖框 → 判定 → 落盘证据 |
| `--pin-demo <prefix>` | 贴图自动验收：拖动/缩放/右键菜单/多张并存/关闭 |
| `--annotate-in` / `--annotate-out` | 标注渲染演示（输入图 → 输出图），供编辑器验收 |
| `--toast-drag-bench <ms>` | 程序化拖动结果浮层，量显示位置与目标的偏差（ISSUE-001） |
| `--bench-capture <path>` | 抓一帧全屏存盘，给上面的拖动基准当观测者 |
| `--probe-portal-screencast` | 验证 portal ScreenCast 授权持久化（restore token） |
| `--probe-portal-no-start` | 同上，但只走到 `SelectSources`（不弹授权框） |
| `--probe-pipewire-lib <n>` / `--probe-pipewire-frames <n>` | 从 portal 的 PipeWire fd 取 n 帧（验证帧源） |
| `--probe-pipewire-out <dir>` | 上面两个探针的落盘目录 |
| `--portal-token-file <path>` | ScreenCast restore token 文件路径 |

## 配置键全表

配置文件：`~/.config/seashot/seashot.conf`，改动**即时生效**（不需要重启常驻实例）。

| 配置键 | 默认值 | 作用 |
| --- | --- | --- |
| `capture/copyAfterCapture` | `true` | 截图后自动复制到剪贴板 |
| `capture/saveDir` | `~/Pictures/Screenshots` | 默认保存目录 |
| `capture/filenameTemplate` | `seashot_%1` | 自动保存的文件名模板（`%1` 会被替换为时间戳） |
| `capture/autoSave` | `false` | 截图后自动保存到默认目录 |
| `capture/openEditorAfterCapture` | `false` | 截图后直接打开编辑器 |
| `capture/keepShadowMargin` | `true` | 直接截取窗口时保留阴影边距 |
| `shortcut/hotkey` | 空 | KGlobalAccel / X11 路径的按键；门户绑定由桌面自己保存 |
| `shortcut/portalAsked` | `false` | 是否已问过"首次运行要不要申请门户授权"（只问一次） |
| `record/fps` | `15` | 录制帧率 |
| `record/format` | `lossless` | 中间格式：`lossless`（FFV1）或 `mp4`（H.264） |
| `ui/trayPersistent` | `true` | 显示常驻托盘图标 |
| `ui/toastAutoHideMs` | `0` | **已失效**：结果浮层在产品路径退役，仅 `--toast-drag-bench` 诊断还在用 |

## 选区内的快捷键

| 按键 | 操作 |
| --- | --- |
| `1`–`9` | 选择、矩形、椭圆、箭头、画笔、高亮、文字、序号、马赛克 |
| `Ctrl+Z` / `Ctrl+Shift+Z` / `Ctrl+Y` | 撤销 / 重做 |
| `Delete` | 删除选中的标注 |
| 方向键 / `Shift` + 方向键 | 微调选区 1 / 10 逻辑像素 |
| `Ctrl+A` | 选取整个虚拟桌面 |
| `Ctrl+C` | 完成并复制 |
| `Ctrl+S` | 完成并保存到默认目录 |
| `Ctrl+T` | 完成并贴图 |
| `Ctrl+L` | 完成并开始长截图 |
| `Ctrl+O` | 完成并对选区做 OCR |
| `Enter` / 双击 / ✓ | 完成，按设置交付结果 |
| `Esc` / 右键 / ✕ | 取消；有面板或编辑状态时先退出当前状态 |

「模糊」工具在工具条里点击选择（没有数字键）；颜色与线宽也在工具条上调整。

## 输出标记（脚本判定用）

程序把可机读的结果打到 **stdout**（日志在 stderr，前缀 `[app]` / `[shot2]` / `[record]` 等）：

| 类别 | 标记 |
| --- | --- |
| 截图 | `CAPTURE ok size=WxH scale=S`、`CAPTURE_ERROR …`、`ANNOTATED count=N size=WxH` |
| 交付 | `SAVED <path>`、`COPIED image/png`、`COPY_ERROR …`、`UI_SILENT …` |
| 长截图 | `SCROLL ok size=WxH`、`SCROLL_REPORT …`、`SCROLL_ERROR …` |
| OCR | `OCR ok lang=… lines=N elapsedMs=M`、`OCR_TEXT_BEGIN` / `OCR_TEXT_END`、`OCR_ERROR …` |
| 录制 | `RECORD ok path=… frames=… dropped=… failed=… elapsedMs=… fps=…`、`RECORD_STOP requested`、`RECORD_EXPORT ok format=gif\|mp4 path=…`、`RECORD_COPY ok mime=image/gif path=…`、`RECORD_FORWARDED`、`RECORD_STOP_FORWARDED` |
| 贴图 | `PIN_CREATED …`、`PIN_CLOSED 剩余=N`、`PIN_COPY ok`、`PIN_SAVE ok path=…` |
| 快捷键 | `HOTKEY_SET …`、`HOTKEY_PORTAL_STATE authorized=0\|1 …`、`HOTKEY_PORTAL_BOUND …` |
| 进程 | `WINDOWS …`、`BACKEND …`、`SEASHOT_READY backend=… hotkey=…`、`FORWARDED_TO_RUNNING_INSTANCE` |
| 验收 | `INPLACE_DEMO …`、`PIN_DEMO …`（各自以 `PASS` / `FAIL` 收尾） |

## 项目结构（速查）

```text
SeaShot/
├── src/
│   ├── app/        # 应用编排、托盘、配置、结果浮层、保存/导出窗口
│   ├── backend/    # Wayland（KWin ScreenShot2）/ X11 截图后端
│   ├── core/       # 公共类型与选区纯逻辑
│   ├── geometry/   # 逻辑坐标 ↔ 物理像素（ScreenMapper，唯一换算点）
│   ├── editor/     # 标注模型、编辑窗口、撤销重做、渲染导出
│   ├── overlay/    # 选区覆盖层 + 就地标注阶段 + 自绘工具条
│   ├── hotkey/     # 全局快捷键（xdg-desktop-portal GlobalShortcuts）
│   ├── scroll/     # 长截图：取帧循环 + 重叠区匹配拼接
│   ├── ocr/        # 文字识别（tesseract）与结果面板
│   ├── record/     # 录制：PipeWire / 轮询、GIF 编码、指示器
│   ├── ui/         # 主题常量与语义图标表
│   └── util/       # 剪贴板、日志
├── resources/      # 应用图标 + 内嵌 KWin 助手脚本
├── tests/          # QtTest 单元测试（11 套）
├── scripts/        # 验收与诊断脚本（13 个 acceptance-*.sh）
├── probes/         # 可重复执行的能力探针（Phase 0 / 长截图拼接等）
├── docs/           # 文档与验收证据
└── install.sh      # 用户级安装脚本
```
