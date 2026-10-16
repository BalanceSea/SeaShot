# SeaShot 实施计划（Phase 0 后提交，待确认）

> 依据：`docs/phase0-findings.md` 的实测结论。本文只描述**打算怎么做**，
> 所有关键路线都标注它建立在哪条实测证据上；推测处一律标注"待验证"。

## 1. 目标与已定决策

独立新项目，C++20 + Qt 6（Quick/QML + QtDBus）+ CMake，双后端（KDE Plasma 6 Wayland / X11）。

已拍定、不再变更的四条：

1. **X11 只做代码级 + 冒烟**：X11 后端必须实现、可编译、能在 XWayland(`:1`) 下跑冒烟；
   本机无 X11 会话（`/usr/share/xsessions` 为空），文档必须标注"**X11 会话未实测**"；
   不安装 X11 会话包、不使用 `sudo`。
2. **窗口截图所见即所得**：虚线框贴合窗口真实外观；截图内容与屏幕所见一致（含标题栏、边框、
   圆角、阴影）——**不允许**用"只有客户区"充数（实测证明无选项的 `CaptureWindow` 正是如此）。
3. **自由矩形选取纳入本期**，与窗口框选共用同一覆盖层。
4. **默认全局快捷键：不注册任何默认键**（你于 Phase 0 后拍定）。理由：实测 `Ctrl+Alt+A` 已被你的
   Spectacle 启动器占用。实现方式：用 KGlobalAccel 注册一个**无默认绑定**的 action（用户可在
   「系统设置 → 快捷键」里自己绑定），同时提供 `seashot --capture` 命令行入口，方便你改成自定义快捷键。

补充（你于 Phase 0 后拍定）：
5. **虚线框贴 `frameGeometry`**（贴窗口实体边界，不贴含阴影的外沿）。
6. **窗口图默认保留阴影边距**（含 alpha），并提供配置项可改为裁到 `frameGeometry`。
7. **接受会话内加载 KWin 助手脚本**，区域截图/录制的自排除走 `excludeFromCapture`。

## 2. 架构

### 2.1 进程与组件

```
seashot（单实例常驻，QApplication + 托盘 + 全局快捷键 + 会话内 D-Bus）
├─ core/            截图会话状态机（选择→截取→后处理→结果）
├─ backend/         CaptureBackend 抽象 + 两个实现
│   ├─ wayland/     KWinBridgeBackend（KWin 脚本 + org.kde.KWin.ScreenShot2）
│   └─ x11/         X11Backend（EWMH + XGetImage + XGrabKey + override-redirect 覆盖层）
├─ overlay/         layer-shell 全屏覆盖层（选择模式 UI：遮罩/悬停虚框/拖拽选区/尺寸提示）
├─ shell/kwin/      seashot-kwin.js（KWin 助手脚本，随进程加载/卸载）
├─ editor/          QML 编辑器（箭头/矩形/椭圆/画笔/文字/撤销重做/颜色粗细）
├─ tray/            KStatusNotifierItem 托盘菜单
├─ settings/        配置界面（Qt Quick）+ QSettings 持久化
└─ recorder/        GIF 录制（Phase 4）
```

单实例：`QDBusConnection` 注册 `org.seashot.SeaShot` + `QLocalServer`；已有实例存在时，
第二次启动只发"开始截图"请求后退出（这样 KDE 快捷键可以简单地指向 `seashot --capture`）。

> 实现取舍（Phase 1 起生效，可复议）：覆盖层与结果浮层用 **QWidget + QPainter** 绘制，
> 编辑器与配置界面再上 Quick/QML。理由：Phase 0 已实测 `QWidget + createWinId() +
> LayerShellQt::Window::get()` 能拿到 layer-shell role（`probes/overlayprobe.cpp`），
> 而覆盖层只是遮罩+虚框+尺寸提示这类 2D 绘制，用 QPainter 的变量最少、迭代最快、
> 绘制与命中测试在同一套坐标里。若你希望覆盖层也用 QML，Phase 2 前可改。

### 2.2 KWin 助手脚本 `seashot-kwin.js`（Phase 1 第一个交付物）

参考 `NextKde/kwin/window-bridge.js` 的成熟做法（信号 → 节流 → `callDBus` 发布），职责：

1. 维护**窗口快照**：`internalId / frameGeometry / clientGeometry / active / minimized /
   resourceClass / caption`，变化时节流推给 `seashot`（实测这些字段都取得到）。
2. **自排除**：把我们自己的窗口（覆盖层、结果浮层、编辑器）设 `excludeFromCapture = true`
   （实测：`w.excludeFromCapture = true` 可写，且能让该窗口从 `CaptureArea` 结果里消失）。
3. 不做窗口状态修改（不 park、不激活），除非后续明确需要。

加载方式：`loadScript(path, "seashot")` + `Scripting.start()`；退出时 `unloadScript("seashot")`。
每个 PID 一组 pluginName，避免与 NextKde 的 `Script0` 冲突。

### 2.3 截图流程（Wayland）

**窗口框选**
1. 热键 → 确保助手脚本已加载 → 显示覆盖层（`LayerOverlay`、四边锚定、键盘 exclusive）。
2. 悬停命中测试用**助手脚本给的 `frameGeometry`**（不是拾色猜测）；虚线框画在 `frameGeometry` 上。
3. 左键单击 → `CaptureWindow(internalId, {include-decoration: true})`。
   **无需隐藏覆盖层**（实测：覆盖层铺满全屏时窗口截图逐字节不变，RMSE=0）。
   **无需等 EOF**（实测：按 `stride*height` 读满即停；等 EOF 会白等 60s）。
4. 结果 → 写剪贴板（`wl-copy`）→ 结果浮层（「编辑」/「另存为」）。

**矩形拖选**
1. 覆盖层上按下拖动 → 选区矩形（同样走坐标转换层）。
2. 因为覆盖层已设 `excludeFromCapture`，理论上**可直接 `CaptureArea` 而覆盖层不入镜**（实测已验证）。
3. 防御路径（若某条捕获路径不吃 `excludeFromCapture`，例如 `CaptureScreen`/录制）：
   隐藏覆盖层 → **等待干净帧再截**：最多 N 次（默认 5 次、间隔 30ms）比对"覆盖层指纹"，
   仍不干净则报错而不是交出脏图（实测：隐藏后 16ms 的截图与"可见时"字节相同 →
   绝不能用"隐藏完立刻截"）。

**像素读取的通用约束**：`asyncCall` + 边读管道边等响应（阻塞式会死锁，实测复现）。

### 2.4 坐标与缩放转换层（唯一转换点）

- 输入有三类坐标：KWin 的**逻辑**坐标（可能是小数，实测 `536.5829…`）、
  ScreenShot2 返回的**物理**像素（`width/height/stride` + `scale`）、Qt/覆盖层的逻辑坐标。
- 只允许一处实现：`geometry::ScreenMapper`
  - `logicalToPhysical(rect, output)`、`physicalToLogical(...)`
  - 取整策略固定为"四舍五入到最近整数，并保留原始浮点用于 1px 级对齐校验"
  - 每个输出（`QScreen` / KWin output）自带 `scale`；跨输出的矩形要按输出切分后再映射
- 本轮实测 scale=1、单输出；**scale≠1 与多输出路径用合成单元测试覆盖**，
  真机验证需要你同意临时改缩放（见第 7 节）。

### 2.5 安装与授权（Wayland 必须满足，否则截图直接被拒）

`install.sh`（用户级、不 sudo）：
1. 二进制 → `~/.local/bin/seashot`
2. `~/.local/share/applications/seashot.desktop`，**必须**包含：

```ini
[Desktop Entry]
Type=Application
Name=SeaShot
Exec=/home/balancesea/.local/bin/seashot
NoDisplay=false
X-KDE-DBUS-Restricted-Interfaces=org.kde.KWin.ScreenShot2
```

   同时用 `QGuiApplication::setDesktopFileName("seashot")` 固定 app_id，
   并保证可执行文件路径与 `Exec` 完全一致（实测含空格路径未通过）。
3. `kbuildsycoca6 --noincremental` 刷新 KService 缓存。
4. 可选 autostart；`seashot --selftest` 做启动自检（授权 / 助手脚本 / `wl-copy` / 后端），
   失败时给出可执行的修复提示而不是静默失败。

### 2.6 设置项（`~/.config/seashot/seashot.conf`，改动即时生效）

| 键 | 默认 | 说明 |
| --- | --- | --- |
| `copyAfterCapture` | `true` | 截图后写入剪贴板（你要求的默认开） |
| `saveDir` | `~/Pictures/Screenshots` | 默认保存目录 |
| `filenameTemplate` | `seashot_%Y%m%d_%H%M%S` | 同名自动加序号，不覆盖 |
| `autoSaveAfterCapture` | `false` | 不点任何按钮时是否自动落盘 |
| `keepShadowMargin` | `true` | 窗口图是否保留阴影边距（含 alpha） |
| `hotkey` | 不绑定 | 默认不注册；KGlobalAccel action 已注册，可在系统设置里自行绑定 |
| `fps` (GIF) | `15` | 可调；时长/体积不设上限 |
| `trayPersistent` | `true` | 常驻托盘 |

## 3. 后端抽象与 X11 实现（代码级 + XWayland 冒烟）

`CaptureBackend` 接口：`listWindows()`、`captureWindow()`、`captureRect()`、
`showOverlay()`、`registerHotkey()`、`excludeFromCapture()`、`startRecording()`（Phase 4）。

X11 实现：
- 窗口枚举：`_NET_CLIENT_LIST_STACKING`、`_NET_FRAME_EXTENTS`、`_NET_WM_WINDOW_TYPE`、
  `_NET_WM_STATE`、`_NET_ACTIVE_WINDOW`（栈序用于悬停命中）+ `XQueryTree` 兜底
- 截图：`XGetImage`（矩形）。**已知限制**：X11 下窗口被遮挡时会截到遮挡物 →
  文档与 UI 中明确说明（如后续需要，再加 XComposite 重定向取窗口自身像素）
- 覆盖层：override-redirect 全屏窗口；自排除：X11 无 `excludeFromCapture` 等价物 →
  隐藏覆盖层 + `XSync` 后截图（X11 下这一路径通常可靠，仍会加"干净帧"重试）
- 热键：`XGrabKey`（含 NumLock/CapsLock 变体掩码）
- 验收：EWMH 解析逻辑用固定字节流做单元测试；`DISPLAY=:1 seashot --backend=x11 --list-windows`
  与截图冒烟；**所有文档与结论一律标注"X11 会话未实测"**。

## 4. 分阶段交付与验收

每阶段结束提交一次，并把命令与原始输出写进 `docs/acceptance/phaseN.md`。

### Phase 1：热键 → 覆盖层 → 框选 → 剪贴板 → 结果浮层

**状态：已交付（2026-10-16）** —— 验收 9/9 通过，记录见 `docs/acceptance/phase1.md`，
复现命令 `bash scripts/acceptance-phase1.sh`。
实施中新增的三条硬约束（D-Bus 服务名按 pid、`excludeFromCapture` 必须精确匹配 appId、
新窗口要自动带标记）与 XWayland 的 MIT-SHM 发现，都记在验收文档里。

任务：单实例与托盘骨架、KWin 助手脚本、ScreenShot2 封装（异步+读满即停）、坐标转换层、
layer-shell 覆盖层、悬停虚框、单击截窗、拖拽截区、`wl-copy` 剪贴板、结果浮层（「编辑」「另存为」）、
`--selftest`、`install.sh`。

验收（全部可执行、可复核）：
1. `seashot --selftest` 全绿（授权 / 助手脚本 / `wl-copy` / 后端）。
2. **覆盖层自排除**：覆盖层可见时 `CaptureArea` 结果中覆盖层像素占比 = 0
   （复用 `probes/` 的品红占比方法）。
3. **所见即所得**：窗口截图在 frame 区域与屏幕同区域逐像素比对 `RMSE < 2%`
   （复用 `probes/wysiwyg-verify.sh` 的方法，阈值按实测 1.6% 量级设定）。
4. **剪贴板持久**：`wl-paste --list-types` 含 `image/png`；**kill 掉 seashot 后**仍可粘贴。
5. **空闲桌面鲁棒性**：连续 20 次区域截图成功率 100%，并记录单次耗时与最坏值
   （实测已发现"空闲桌面可能返回陈旧帧"，因此这项必须留证据）。
6. **XWayland 冒烟**：`DISPLAY=:1 seashot --backend=x11 --selftest` 通过 + 能截到 XWayland 窗口；
   验收文档标注"X11 会话未实测"。

### Phase 2：编辑器
箭头、矩形、椭圆、自由画笔、文字（可拖动/调整）、撤销/重做、颜色与粗细、复制、保存/另存为。
验收：撤销栈与几何命中的单元测试；导出尺寸 = 原图尺寸；退出未保存不留下半成品文件。

### Phase 3：配置界面 + 托盘

**状态：已交付（2026-10-16）** —— 验收 7/7 通过，记录见 `docs/acceptance/phase3.md`，
复现命令 `bash scripts/acceptance-phase3.sh`。新增 CLI：`--settings`、`--set-hotkey`、
`--hotkey-status`、`--print-config`。

第 2.6 节全部设置项 + 即时生效 + 持久化 + 快捷键冲突提示 + 托盘菜单（截图/GIF/设置/退出）。
验收：改设置立刻生效、重启后保持、恢复默认、快捷键被占用时给出明确提示。

### Phase 4：GIF 录制
只录选定区域；帧率默认 15 可调；**无时长与体积上限**；停止方式：热键/托盘/浮层按钮；
帧采集自己做，编码用已装的 `ffmpeg`（palettegen/paletteuse 两遍）。
录制来源优先级：
1. `org.freedesktop.portal.ScreenCast`（PipeWire 流；**待验证**：会弹授权对话框，
   需要你配合点一次；选区裁剪在客户端按坐标转换层做）
2. 若 portal 在某路径不可用 → 明确报错并给出原因，**不静默降级**。
   X11 侧用 `ffmpeg -f x11grab`（已实测可跑）。
验收：`ffprobe` 校验输出尺寸与帧率等于设置值；录制中 UI 不卡顿（有可观测的丢帧统计）；
停止后文件可打开；磁盘写失败要可见。

## 5. 风险与回退

| 风险 | 依据 | 回退 |
| --- | --- | --- |
| 授权依赖已安装 desktop 文件 + sycoca 生效 | 实测 NoAuthorized → 装文件后通过 | 启动自检报错并提示 `kbuildsycoca6`；必要时走 portal `Screenshot`（会有授权对话框） |
| 空闲桌面返回陈旧帧 | 实测隐藏后 16ms 的截图与"可见时"字节相同 | 干净帧重试；失败即报错，不交脏图 |
| `excludeFromCapture` 在某条捕获路径不生效 | 仅在 `CaptureArea` 上验证过 | 回退"隐藏 + 等待 + 重试"；Phase 1 验收覆盖所有实际用到的路径 |
| 分数缩放 / 多输出未实测 | 本机单输出 scale=1 | 合成单测覆盖；需要真机验证时请你临时改缩放 |
| 阴影边距随装饰主题变化 | 实测 Breeze：left65/top53/right65/bottom77 | 不硬编码：用返回图像的 alpha 不透明包围盒运行时推导（±1px），或读主题配置 |
| portal 录制需用户授权、体验未知 | 未验证 | 首次录制前解释清楚并引导；失败给出可读错误 |
| 与 NextKde 的 KWin 助手脚本互相干扰 | 两套脚本共用 `/Scripting` | 独立 pluginName（`seashot`）、不做窗口状态修改、退出即卸载 |
| 多实例同时截图 | Phase 1 验收踩到过：固定 D-Bus 服务名让第二个进程初始化直接失败 | 服务名与脚本文件按 pid 区分（`org.seashot.SeaShot.p<pid>`） |
| **被 kill/崩溃后 KWin 助手脚本残留** | Phase 2 后踩到过：残留脚本里的自排除状态会继续把之后所有 SeaShot 窗口标成「排除出截图」，导致浮层/覆盖层再也截不到，排查耗时很久 | ① 启动时扫描缓存目录清理死进程的脚本；② SIGTERM/SIGINT 优雅退出并卸载脚本 |
| 结果浮层在全屏应用前面看不见 | Phase 2 后实测：普通 `Qt::Tool` 窗口压在全屏窗口之下（且 kwin 的窗口列表里能看到但屏幕上被盖） | 浮层改用 layer-shell（`LayerOverlay`），位置用「锚 top\|left + 边距」精确摆放 |
| 自排除误伤第三方窗口 | Phase 1 踩到过：子串匹配把 `seashot-probe-window` 也排除了，而 KWin 对已排除窗口会**取消**截图 | 改为精确 appId 匹配（`seashot` / `seashot.*`） |
| XWayland 下 `GetImage` 返回 BadMatch | Phase 1 实测 code=8 major=73（root 与子窗口均如此），MIT-SHM 正常 | X11 后端优先 MIT-SHM，退回 GetImage；错误信息带 X 错误码 |
| 快捷键冲突 | 实测 `Ctrl+Alt+A` 属 Spectacle | 见第 7 节待裁决 1 |

## 6. 明确不做（本期）

多屏视频拼接、滚动长截图、OCR、云上传、GNOME/其他合成器（非 KDE 的 Wayland）、
X11 会话真机验收、Wayland 下"窗口被遮挡也截到窗口自身"以外的 X11 增强（XComposite）。

## 7. 待你裁决（已在 Phase 0 后确认，此处仅留档）

1. **全局快捷键** → 选 (c)：**默认不注册任何键**，由 KGlobalAccel action + `seashot --capture` 供你自行绑定。
2. **虚线框贴合范围** → 贴 `frameGeometry`。
3. **窗口图阴影边距** → 默认保留（可配置为裁剪）。
4. **区域截图/录制的自排除** → 接受加载 KWin 助手脚本。

---

**Phase 0 到此结束。按你的要求，在拿到这份 PLAN.md 的确认（以及第 7 节四条裁决）之前，我不会写任何应用代码。**
