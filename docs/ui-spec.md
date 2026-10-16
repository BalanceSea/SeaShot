# SeaShot 视觉规格（ISSUE-004 第一版，已落地）

> 目的：把"好用但平"的界面统一到一套可复述的规格上，并且**全部跟随系统主题**
> （颜色一律取自 `QPalette`，尺寸一律取自 `src/ui/Theme.h` 的 `spec::` 常量）。
> 规格集中在一处，改一个数值就能整体调整。

## 1. 规格表

| 项 | 值 | 位置 |
| --- | --- | --- |
| 卡片圆角 | 8px | `spec::cardRadius` |
| 按钮圆角 | 4px | `spec::buttonRadius` |
| 卡片内边距 | 12px | `spec::cardPadding` |
| 元素间距 | 6px（分组内 8px） | `spec::gap` |
| 底部操作条高 | 34px | `spec::barHeight` |
| 按钮高 / 左右内边距 | 24px / 10px | `spec::buttonHeight` / `spec::buttonPadX` |
| 图标尺寸 | 16px（头部 32px） | `spec::iconSize` |
| 卡片描边 | 外圈 2px 深色(α100) + 内圈 2px 主题高亮(α235) | `spec::frameOuter/Inner*` |
| hover 态 | 底色提亮 8% | `spec::hoverLighten` |
| pressed 态 | 底色压暗 18% | `spec::pressedDarker` |
| 主按钮 | `QPalette::Highlight` + `HighlightedText` | `ui::cardColors()` |
| 次要按钮 | `QPalette::Button` + `ButtonText`，1px `Mid` 描边 | 同上 |
| 说明文字 | 正文 × 0.92，`PlaceholderText` 色 | `hintLabel()`（配置界面） |
| 标题 | 正文加粗 × 1.15 | 配置界面头部 |

图标全部来自系统图标主题（Breeze），并带候选回退；取不到就退回纯文字（不会出现空白按钮）。
语义图标集中在 `ui::iconXxx()`，换主题只改一处。

| 语义 | 图标名（回退顺序） |
| --- | --- |
| 编辑 | `document-edit` → `edit-entry` → `edit` |
| 另存为 | `document-save-as` → `document-save` |
| 复制 | `edit-copy` → `edit-duplicate` |
| 关闭 | `window-close` → `dialog-close` |
| 录制 / 停止 | `media-record` / `media-playback-stop` |
| 截图 / 设置 | `camera-photo` / `settings-configure` |
| 撤销 / 重做 / 删除 / 保存 / 刷新 | `edit-undo` / `edit-redo` / `edit-delete` / `document-save` / `view-refresh` |
| 编辑器工具 | 选择 `transform-move`、箭头 `draw-arrow`、矩形 `draw-rectangle`、椭圆 `draw-circle`、画笔 `draw-freehand`、文字 `draw-text` |
| 选择目录 | `folder-open` → `document-open` |

## 2. 这一版落地了什么

### 结果浮层（`src/app/Toast.cpp`）
- 按钮从"纯色文字块"改成 **图标 + 文字 + hover/pressed 态**，主按钮（编辑）用主题高亮色；
- 按钮尺寸按"图标 + 文字 + 内边距"计算，不再挤在一起；间距/圆角取自规格；
- 描边保留（你先前要求"容易找到截图"），但按规格统一为 2px 外圈 + 2px 内圈；
- 悬停跟踪：鼠标移到按钮上会高亮，按下会压暗（`m_hoverButton` / `m_pressedButton`）。

### 录制指示器（`src/record/RecordingIndicator.cpp`）
- 与浮层同一套卡片描边/圆角规格；停止按钮加图标；
- 闪烁红点保留（录制中的强提示）。

### 编辑器工具栏（`src/editor/EditorWindow.cpp`）
- 工具按钮加图标（六种工具）并改成"图标 + 文字"；
- 撤销/重做/删除/复制/保存/另存为 全部加图标；
- 工具栏按"工具 → 颜色/粗细 → 撤销重做删除 → 复制保存另存为"分组，保留分隔线。

### 配置界面（`src/app/SettingsDialog.cpp`，重做布局）
- 头部：应用图标 + 标题 + 一句说明；
- 五个分组（截图后 / 保存 / 界面 / 全局快捷键 / GIF 录制），统一内边距与标签右对齐；
- 「默认保存目录」行改为 输入框 + 清除按钮 + **文件夹图标按钮**（原来是一个"浏览…"文字按钮）；
- 说明文字统一为小一号的次要色（不再和控件混在一起）；
- 底部：左侧次要色显示配置文件路径，**按钮右对齐**（`恢复默认`=↺、`关闭`=✕ 且为默认按钮）；
- 原有"改动立即生效"行为不变。

## 3. 取证

| 界面 | 证据 |
| --- | --- |
| 配置界面 | `docs/acceptance/evidence/ui-settings-v1.png`（用 `SEASHOT_UI_GRAB=<path> seashot --settings` 直接渲染控件取证，不受其它窗口遮挡影响） |
| 结果浮层 | `docs/acceptance/evidence/ui-toast-v1.png`（按钮已带图标：✏ 编辑 / 另存为 / 复制 / ✕ 关闭） |

## 4. 还没做 / 想听你意见

1. **浮层投影**：layer-shell 表面不能像普通窗口那样让合成器投阴影，只能自己画"假阴影"
   （多画几层半透明圆角矩形，或把表面做大一点留出阴影边距）。要不要？会增加一点拖动开销。
2. **深色主题复核**：我只能在 BreezeLight 下取证（你的当前配色）。切到 BreezeDark 后
   若某处对比不够（尤其描边与次要按钮），告我一声我再调。
3. **编辑器画布**：现在是"按窗口自适应缩放"，没有缩放/平移/滚动条；需不需要加？
4. **配置界面更进一步**：左侧分组导航 + 右侧页面（KDE 系统设置那种）；现在是纵向堆叠。
   四五个分组堆叠其实还行，做导航会多一层代码。
5. **托盘图标**：现在用主题的 `camera-photo`；要不要换一个更"SeaShot"的图标（自绘或选别的）？
6. 高 DPI：所有绘制都用逻辑坐标 + `devicePixelRatio` 的缓存 pixmap，理论上跟随缩放；
   若你改了缩放发现模糊/错位，告我。

## 5. 第二版（按你的四条答复落地）

你的答复：**不要假阴影** / **不用复核深色主题** / **编辑器要缩放平移** / **配置界面改左侧导航** /
**图标用你给的那张图**。实现如下：

### 应用图标（`resources/icons/seashot.png`，内嵌资源）
- 你给的 `~/Downloads/gpt-image-3-0.png`（2048×2048）缩到 512×512 后**收进项目**并嵌入 Qt 资源
  （`:/seashot/resources/icons/seashot.png`），所以安装后不依赖 Downloads 目录；
- 用在三处：**托盘图标**、**配置界面头部**、**所有窗口的图标**（`QGuiApplication::setWindowIcon`）；
- 资源取不到时回退到主题的 `camera-photo`。

### 编辑器画布：缩放 / 平移 / 滚动条
- 画布放进 `QScrollArea`（自适应窗口尺寸、居中显示、超出即出滚动条）；
- **缩放**：工具栏 `适应窗口`（勾选态）/ `−` / 百分比 / `＋` / `100%`；
  快捷键 `Ctrl+0` 适应窗口、`Ctrl+1` 100%、`Ctrl+=`/`Ctrl++` 放大、`Ctrl+-` 缩小；
  **`Ctrl+滚轮`** 缩放（不按 Ctrl 时滚轮正常滚动）；
- **平移**：**中键拖动**（把位移交给滚动条），滚动条本身也可用；
- 关键约束：**标注仍然只存图像坐标** —— 缩放只影响显示映射，所以导出尺寸、命中测试、
  手柄拖拽在任意缩放下都正确（有单测覆盖：`canvasZoomScalesCoordinateMapping` 等 3 例）；
- 默认窗口从 900×600 放宽到 **1180×760**：之前工具栏会被挤进右侧 ">" 溢出菜单里。

### 配置界面：左侧导航 + 右侧页面
```
[icon] SeaShot
       截图、编辑与 GIF 录制 —— 改动立即生效并写入配置文件
┌────────────┬───────────────────────────────────────┐
│ 截图后      │  截图后                                │
│ 保存        │  截完图之后 SeaShot 自动做什么           │
│ 界面        │   ☑ 自动复制到剪贴板                    │
│ 全局快捷键   │   ☐ 直接打开编辑器（不再弹结果浮层）      │
│ GIF 录制    │   ☐ 自动保存一份到默认目录               │
└────────────┴───────────────────────────────────────┘
配置文件 …/seashot.conf                     [↺ 恢复默认] [✕ 关闭]
```
- 左侧 `QListWidget`（带图标）+ 右侧 `QStackedWidget`，五页与原来五组一一对应；
- 每页是"标题 + 一句说明 + 表单"，不再用重边框的 `QGroupBox`；
- 底部按钮右对齐、配置文件路径左下角次要色；
- **所有控件的 objectName 保持不变**，所以 Phase 3 的配置界面单测（5 例）无需改动即通过。

### 没做（按你的答复）
- 浮层的"假阴影"：不做（你选了不要）。
- 深色主题复核：不做（你选了不需要）。规格里所有颜色都取自 `QPalette`，切换主题会自动跟随。
