# 已知问题 / 待办（按反馈记录）

> 规则：你反馈的问题先进这里（含复现方式、已试过什么、下一步候选方案），
> 修完再把它挪到 `docs/acceptance/` 对应阶段的记录里并标注修复提交。

## 待修

### ISSUE-004 整体 UI 待美化

- **状态**：待办（等你给具体意见；下面是现状与我看到的问题）
- **反馈原话**：「现在开 phase4 先记录一下 UI 需要美化」（2026-10-16）
- **现状**：
  - 配置界面：QWidget + `QGroupBox`/`QFormLayout` 默认样式，能用但平；没有图标；
    「保存目录」行里输入框与「浏览…」紧贴；说明文字与表单项混在一起；
    「恢复默认 / 关闭」按钮靠左（没靠右下角）；分组间距与留白没有节奏。
  - 结果浮层：自绘卡片，描边偏重（深色环 + 高亮环两层），按钮是纯色圆角块、没有图标，
    按钮没有 hover/pressed 状态，状态文字与按钮挤在同一行。
  - 录制指示器（Phase 4 新增）：同为自绘风格，需要与浮层统一。
  - 编辑器工具栏：`QToolBar` 纯文字按钮、无图标；颜色按钮只有一个色块。
  - 所有界面都没用 Breeze 图标（可用 `QIcon::fromTheme`：camera-photo / edit /
    document-save-as / edit-copy / window-close / media-record 等）。
- **候选改进（等你定优先级）**：
  1. 统一视觉规格：圆角、描边、内边距、控件间距、字号层级；
  2. 用 Breeze 主题图标（`QIcon::fromTheme` + fallback）替代纯文字按钮；
  3. 配置界面重构：分组卡片 + 按钮右对齐 + 说明文字与控件分离 + 目录行做成
     「输入框 + 文件夹图标按钮」；
  4. 浮层：降低描边重量、按钮加 hover/pressed/禁用态与图标、录制中在浮层上给一个
     「转为录制」按钮；
  5. 编辑器：工具栏图标 + 工具分组分隔线 + 当前颜色/粗细的图形预览；
  6. 深色/浅色主题、高 DPI（`devicePixelRatio`）与字体缩放都要实测；
  7. 可选：配置界面/编辑器改用 QML（PLAN 原话提过），代价是重写现有控件逻辑。
- **下一步**：我先出一版视觉规格表（间距/字号/图标/状态），你确认后再动代码；
  改完用截图对比存档到 `docs/acceptance/evidence/`。

### ISSUE-006 录制改走 PipeWire 直读（更高帧率 / 更大区域）

- **状态**：待办（未要求；当前轮询实现已满足“15fps / 只录选区 / 无上限”）
- **背景**：Phase 4 用“定时调 ScreenShot2 采帧”实现（零依赖、无授权弹窗，实测 15.19fps、
  0 丢帧）。需要更高帧率或更大区域时，这条路会撞到“每帧一次 D-Bus 往返 + 定时器抖动”。
- **可行性（已实测环境）**：本机 `libpipewire-0.3` 1.6.9、`gstreamer-1.0` 1.28.8、
  `gst-launch-1.0` + `pipewiresrc` 均具备；缺的是 portal ScreenCast 会话协商
  （`CreateSession`/`SelectSources`/`Start` + `OpenPipeWireRemote`）与帧拷贝。
- **代价**：新依赖（pipewire 或 gst）、流线程/协商代码、**每次会话要用户授权弹窗**。
- **建议做法**：保留当前轮询实现作为默认/回退；新增 `record/backend=pipewire` 选项，
  录制前弹一次授权，后续会话用 restore token 免弹。

## 已修（留档，避免回归）

### ISSUE-001 结果浮层拖动卡顿

- **状态**：已修（提交见 `docs/acceptance/issue-001-drag.md`；回归基准 `scripts/benchmark-toast-drag.sh`）
- **反馈**：「拖动截图窗口还是会卡顿」（2026-10-16，第二次反馈）
- **根因（实测）**：浮层靠 layer-shell 的 `set_margin` 定位，而 `LayerShellQt::Window::setMargins()`
  只把新边距放进待提交状态，**需要一次 surface commit 才生效**。拖动时我们只改边距、不产生新缓冲，
  于是 KWin 只在"碰巧有别的提交"时才应用新位置 → 位置以秒级间隔成块跳动。
- **修法**：`setMargins()` 后显式 `update()` 促成提交（卡片有 QPixmap 缓存，代价一次 blit ~70µs）；
  同时去掉 8ms 合并提交定时器（它反而加延迟），改为逐事件提交 + 跳过重复位置。
- **实测**：lagPx 平均 269 → **0**，最大 564 → **0**。
- **回归判据**：250px/s 拖动下，显示位置与目标位置偏差 ≤ 8px（≈32ms）。

### ISSUE-007 同一进程的截图请求拍不到自己的 layer-shell 表面（观测记录）

- **状态**：非 bug，但反直觉，留档免得后人（包括我自己）再踩
- **现象**：进程 A 显示浮层（layer-shell）后，**A 自己**调用 ScreenShot2 `CaptureArea`
  得到的帧里没有浮层；**进程 B** 同时抓同一块屏幕却能拍到（自排除已确认关闭、
  KWin 快照里该窗口 `excludedFromCapture=false`）。
- **实测细节**：A 的帧尺寸/内容正常（均值随浮层出现而变化），但浮层品红诊断边框一个像素都没有；
  B 的帧能在预期坐标找到完整品红框（bbox 与浮层尺寸一致）。
- **影响**：
  - 好处：录制（Phase 4 在同进程抓帧）天然不会录到自己的覆盖层/指示器；
  - 影响：**不能**用"同进程抓帧"来自检自己的浮层外观 —— 需要子进程观测
    （ISSUE-001 的基准就是这么做的：`--bench-capture`）。
- **待查**：是否是 KWin 对"请求方自己的 layer-shell 表面"的特例处理（未深入 KWin 源码）。

### ISSUE-002 结果浮层在全屏应用前面曾经不可见

- **状态**：已修（提交 `d0bbfc2`）
- **成因**：普通 `Qt::Tool` 窗口在 Wayland 下压不过全屏窗口。
- **修法**：浮层改用 layer-shell（`LayerOverlay`）+「锚 top|left + 边距」定位。
- **留档原因**：修 ISSUE-001 时别退回普通窗口，否则这个会复发。

### ISSUE-003 被 kill/崩溃后 KWin 助手脚本残留

- **状态**：已修（提交 `d0bbfc2`）
- **成因**：进程被 SIGKILL/崩溃时来不及 `unloadScript`，残留脚本里的"自排除"状态会继续
  把之后所有 SeaShot 窗口标成"排除出截图"，表现是浮层/覆盖层再也截不到。
- **修法**：启动时扫描缓存目录清理死进程脚本 + SIGTERM/SIGINT 优雅退出并卸载。

### ISSUE-005 托盘菜单析构 double free

- **状态**：已修（提交 `525b7d6`，Phase 3）
- **成因**：`Tray` 析构里手动 `delete m_menu`，但 `KStatusNotifierItem` 已接管 contextMenu
  所有权 → double free。生产影响：配置界面里关掉「常驻托盘」就会崩。
- **修法**：不手动删菜单，交给 item 管理；由新增的 `tst_tray` 单测兜住。
