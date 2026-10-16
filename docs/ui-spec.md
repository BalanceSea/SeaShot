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
