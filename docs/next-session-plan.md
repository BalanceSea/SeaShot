# SeaShot 交接计划（2026-10-16 收工，2026-10-17 继续）

> 本文是**自包含**的：明天开新对话时，读这一份即可接上，不依赖上一轮对话上下文。
>
> **一句话现状**：**用户最初提的 8 条诉求（①就地编辑 ②工具条 ③1:1 ④可改选区 ⑤快捷键
> ⑥贴图 ⑦长截图 ⑧OCR）全部完成并验收**，原有 Phase 1~4（截图/编辑器/配置/录制）也仍然全绿 ——
> 全套验收：`phase1` 9/0、`phase2` 4/0、`phase3` 7/0、`phase4` 7/0、
> `phaseA1` 6/0、`phaseA2` 10/0、`phaseA3` 13/0、`phaseA3-color` 7/0、`phaseB` 11/0、
> `phaseC` 10/0、`phaseD` 8/0、`phaseE` 7/0，外加 **11 套单测**。
>
> 剩下的都是小尾巴（不阻塞）：A4（工具条上的复制/保存按钮）、颜色/粗细持久化，
> 以及 Phase D 里几条**没实测过**的路径（见 `docs/acceptance/phaseD.md` 文末）。

---

## 0. 明天下手第一件事

**先让用户自己完整体验一遍**（这是唯一没被自动化覆盖的部分）：
按热键 → 框选 → 就地标注（画/改颜色粗细/拖手柄改选区）→ `Ctrl+C`/`Ctrl+S`/`Ctrl+T` 贴图 /
`Ctrl+L` 长截图 / `Ctrl+O` OCR → 看结果浮层。

之后按用户反馈挑小尾巴做：
1. **A4**：工具条尾部加「复制」「保存」两格（`ConfirmAction` 管道已通，只剩加格子）。
2. **颜色/粗细持久化**（现在只在一次会话内有效，默认 `#ff4040` / 4px）。
3. 颜色/粗细的**自定义取色器**（现在只有 10 个预设色）。

开始前请先确认一件事（外部依赖，不阻塞 A4，但影响 Phase E）：

```fish
# OCR 语言包（本机只有 afr/osd，缺中英文）
# ✅ 2026-10-17 已确认装好：chi_sim 与 eng 都在
tesseract --list-langs    # afr / chi_sim / eng / osd
```

---

## 1. 项目是什么

**SeaShot**：KDE Plasma 6 下的 Linux 截图 + 录制工具（Wayland 主、X11 备）。
路径 `/home/balancesea/Vibe Code/SeaShot`，C++20 + Qt6（Quick 未用，纯 Widgets）+ LayerShellQt。

核心能力（**均已实现并验收**）：

| 能力 | 状态 |
| --- | --- |
| 全局快捷键 → 覆盖层框选（自动框窗口 + 自由矩形） | ✅ Phase 1 |
| 复制到剪贴板 / 保存 / 另存为 | ✅ |
| 结果浮层（缩略图卡片，可拖动） | ✅ |
| 编辑器（矩形/椭圆/箭头/画笔/高亮/文字/序号/模糊/马赛克 + 撤销重做） | ✅ Phase 2 |
| 配置界面 + 托盘 + 快捷键设置 | ✅ Phase 3 |
| GIF 录制（PipeWire 取帧，轮询回退） | ✅ Phase 4 |
| MP4 录制 / 导出 GIF / 复制 GIF | ✅（后加） |
| 录制格式可选（无损 / MP4，配置界面） | ✅（后加） |

验收脚本与证据：`scripts/acceptance-phase{1,2,3,4}.sh`、`docs/acceptance/`。
**改任何东西后，至少要保证 `scripts/acceptance-phase1.sh` 仍然全绿**（它覆盖截图主链路）。

---

## 2. 明天要做的改造：就地编辑（对齐 QQ 截图 / PixPin）

用户原话：「编辑图片请参考 QQ 的图片编辑或 PixPin 的，现在的图片编辑达不到我想要的效果」

**根因判断（已与用户确认）**：主要矛盾不是美化，而是**编辑位置**——
QQ/PixPin 是选完区域**直接在屏幕上标注**（不打开新窗口），而 SeaShot 现在打开一个独立
`EditorWindow`（深色画布、图居中、顶栏宽工具条）。这是**架构差异**。

### 用户确认的 8 项诉求

1. **就地编辑**（核心，架构级）
2. 工具条紧凑化（图标为主）
3. 图片按 **1:1** 显示，不缩放
4. 选完可**继续调整选区**，标注不丢
5. 快捷键对齐 QQ/PixPin
6. **贴图/钉图到屏幕**
7. **长截图（滚动截屏）**
8. **OCR 文字识别**

完整差距分析与架构方案见 **`docs/plan-inplace-editor.md`**（本文件的第 3、4 节是它的执行摘要）。

---

## 3. Phase 0 已完成（结论，不必重做）

探针：`probes/inplace-probe.cpp`（独立程序，**不碰主链路**）。

| 判据 | 实测 | 结论 |
| --- | --- | --- |
| 1:1 绘制 | `surface=1920x1080 frozen=1920x1080` | ✅ 无缩放 |
| 连续拖拽流畅度 | `avgMoveGapMs=1.5531`（≈645 次/秒） | ✅ 流畅 |
| 自绘工具条命中 | `toolbarHits=13`，index **0–9 全部命中** | ✅ 无漏点 |
| 工具条不误触发标注 | 5 次 `ANNOTATION_DONE` 全在工具条外 | ✅ 点击策略成立 |
| layer-shell 真生效 | 全屏应用前面仍在最前（用户目视确认） | ✅ |
| 是否真丢帧 | 唯一一次 `maxMoveGapMs=134` 经确认是**手动停顿** | ✅ 无丢帧 |

### ⚠️ 从这里继承一条硬规则（踩过坑，务必遵守）

> **任何 layer-shell 表面，必须在 `show()` **之前** 调用 `ensureLayerShell()`。**
>
> 否则报 `qt.qpa.wayland: Cannot set shell integration while there's already a shell surface created`，
> 而且 **`layerShell=1` / `get()` 返回非空都是"假成功"** —— 表面实际仍是普通窗口，
> 会被全屏应用盖住。探针第一版就栽在这（顺序写反）。
>
> 主项目 `Toast` 因为经由 `reposition()` 在 `show()` 之前调用，所以一直是对的，可作范例。

---

## 4. Phase A：就地编辑骨架（明天从 A1 开始）

### 复用什么（**不要重写**）

| 文件 | 复用方式 |
| --- | --- |
| `src/overlay/Overlay.{h,cpp}` | 已有 layer-shell 全屏表面 + 完整框选（含 `Intent{Screenshot,Record}`）。**A1 已加 Annotating 阶段**（1:1 冻结屏幕 + 选区/手柄 + 确认/取消） |
| `src/editor/Annotation.{h,cpp}` | 标注数据模型（图像坐标，9 种工具已全） |
| `src/editor/AnnotationDocument.{h,cpp}` | 增删改 + 撤销/重做 |
| `src/editor/AnnotationRenderer.{h,cpp}` | `renderAnnotations()` 是**唯一导出出口**；模糊/马赛克的"预览与导出同一实现"已就位，别绕过 |
| `src/app/Toast.{h,cpp}` | **自绘控件的范例**：缓存位图 + `QWindow::setMask` 限输入区 + 脏区重绘 |

### 分步（每步都要给用户可验证的结果，禁止一次性大坨）

| 步 | 内容 | 用户怎么验 | 状态 |
| --- | --- | --- | --- |
| **A1** | `Overlay` 增加 Annotating 阶段：选完**不退出**，1:1 绘制冻结屏幕 + 选区边框/手柄 | 拖框后**不弹新窗口**，屏幕上出现带手柄的选框 | ✅ **已完成**（2026-10-17，6/6；判据 1–5 含逐像素比对，见 `docs/acceptance/phaseA1.md`） |
| **A2** | 自绘紧凑**图标**工具条（贴选区下方，下方放不下则上方）+ 工具切换与高亮 | 点工具能切、当前工具高亮 | ✅ **已完成**（2026-10-17，10/10；含 10 按钮全命中、`1`–`9` 切换、hover、自动避让，见 `docs/acceptance/phaseA2.md`） |
| **A3** | 标注绘制/命中/撤销接到 `AnnotationDocument` | 能画矩形/箭头，`Ctrl+Z` 生效 | ✅ **已完成**（2026-10-17，13/13；含「预览==导出逐像素 0 差异」与「完成→结果图真的有标注」端到端，见 `docs/acceptance/phaseA3.md`） |
| **A3.1** | 颜色 / 粗细（自绘弹出面板：色板 + 滑杆） | 选色/拖粗细，新画的标注跟着变 | ✅ **已完成**（2026-10-17，7/7；面板不压选区、面板内空白点击不误画，见 `docs/acceptance/phaseA3-color.md`） |
| **A4** | 工具条上的复制/保存按钮接现有链路 | **`acceptance-phase1.sh` 必须仍全绿** | ⬅ **下一步**（管道已在 Phase B 打通，只剩加两格） |
| **C** | 贴图/钉图到屏幕（layer-shell，可拖动/缩放/右键菜单/多张并存） | 钉图后切到全屏应用仍可见；关闭无残留 | ✅ **已完成**（2026-10-17，10/10；含"另一进程截屏按纯色块核对"与"全屏白底参照之上仍可见"，见 `docs/acceptance/phaseC.md`） |

### A1/A2/A3 已落地的实现要点（A4 直接接在这些之上）

- `Overlay` 内部多了 `Stage{Selecting,Annotating}`；`startSelection()` 复位，
  `beginAnnotating(selection, frozenPhysical, scale)` 进入标注阶段（冻结图会被归一到**逻辑像素**）。
- 新增信号 `annotationConfirmed(SelectionResult)` / `annotationCancelled()`；
  新增开关 `setAnnotationEnabled()`（**只有交互式截图打开**，脚本式调用一律 false）。
- `Application::enterAnnotationPhase()` 负责抓冻结屏幕（自排除可用时直接抓；否则先藏覆盖层再等 250ms），
  `Application::finishSelectionPhase()` 负责收尾（快照标注 + 隐藏 + 按需等一帧 + `performCapture()`）。
- **标注坐标 = 选区图像坐标**（原点 = 选区左上角）。所以：
  - 预览 = `renderAnnotations(m_frozenSelection, items)` 的输出（`m_composedSelection`）；
  - 导出 = 对抓到的选区图再调一次同一函数；两边**逐像素一致**（A3 判据 7 实测 0 差异）。
  - 分数缩放（scale≠1）时导出路径需要把标注坐标乘以输出 scale —— 本机 scale=1 无法实测，
    `ScreenMapper` 是既有的唯一换算点，真机验证放进 Phase B/后续。
- 工具条**自绘**且几何只有一处真源：`toolOrder()/buildBarCells()/toolbarSize()/toolbarRect()`
  → `toolButtons()/actionButtons()/cellIndexAt()`。
- 标注交互（绘制/命中/移动/改尺寸/文字输入/撤销）都是 Overlay 自绘自管，
  **语义对齐 `AnnotationCanvas` 但不用子控件**；文字输入是自己吃键盘画的（含光标）。
- `setMouseTracking(true)` 是 A2 的必要条件（否则不按键时收不到 `mouseMove`，hover 全失效）。
- 自检/验收入口：`seashot --inplace-demo <prefix>` —— 程序化拖框 → 跑完 A1/A2/A3/A3.1 全部判据 →
  落盘 `<prefix>-overlay.png` / `-frozen.png` / `-annotated.png` / `-panel-color.png` / `-panel-width.png`；
  加 `SEASHOT_INPLACE_DEMO_COMMIT=1` 则最后合成 `Enter`（完成），把"标注进结果图"也跑一遍。

### ⚠️ A4 的关键约束（不做就会被验收打回）

> **标注阶段只对「交互式截图」开启；脚本式调用（`--rect` / `--window` / `--save` / `--capture-screen`）
> 必须保持现有行为完全不变**，否则 Phase 1 验收与所有自动化会全崩。

### 工具条为什么自绘而不是用子控件

layer-shell 上子控件的输入区/合成在本项目反复咬人（见 `docs/known-issues.md` 的 ISSUE-001 拖动卡顿、
ISSUE-007 自排除）。`Toast` 用自绘走通了，照它做。工具条命中判定要点（Phase 0 已验证可行）：

1. 工具条矩形**优先**于标注逻辑判定
2. 落在工具条矩形内但不在按钮上 → 也算"工具条"，**不开始标注**
3. 图标、hover/按下态自己画；提示气泡可后补（A2 已补：悬停显示工具名 + 数字键）
4. 顺序坑：不调 `setMouseTracking(true)` 就收不到不带按键的 `mouseMove` → hover 全失效

### 状态机

```
Idle → Selecting（拖框 / 悬停高亮窗口）
     → Annotating（A1：1:1 冻结屏幕 + 选区/手柄；A2：自绘图标工具条 + 1–9 切换；
                   A3：画标注 + 命中/移动/改尺寸 + 撤销；A3.1：颜色/粗细；
                   B：8 手柄/4 边拖改选区 + 方向键微调 + Ctrl+A/C/S；A4：工具条上的复制/保存）
     → Done（复制 / 保存 / 贴图 / 进编辑器 / 取消）
```

标注存**图像坐标**（与 `Annotation` 现有约定一致），所以"1:1 显示"和"选区可调整"都是自然结果：
拖选区只改图像原点映射，标注数据不动。

---

## 5. 后续阶段（摘要，详见 `docs/plan-inplace-editor.md`）

| 阶段 | 内容 | 备注 |
| --- | --- | --- |
| ~~**B**~~ | ~~8 手柄拖改选区 + 快捷键对齐 QQ/PixPin~~ | ✅ **已完成**（2026-10-17，11/11，见 `docs/acceptance/phaseB.md`） |
| ~~**C**~~ | ~~贴图/钉图到屏幕~~ | ✅ **已完成**（2026-10-17，10/10；`--pin` 与就地编辑 `Ctrl+T` 两个入口，见 `docs/acceptance/phaseC.md`） |
| ~~**D**~~ | ~~长截图（滚动截屏）~~ | ✅ **已完成**（2026-10-17：探针 `docs/acceptance/phaseD-probe.md` + 正式实现 `docs/acceptance/phaseD.md`，真实屏幕帧 8/8；`--scroll-capture` 与 Ctrl+L 两个入口） |
| ~~**E**~~ | ~~OCR~~ | ✅ **已完成**（2026-10-17：命令行走 tesseract stdin/stdout，界面走结果面板 + 复制全部/选中；中文合成图 10/10 命中 390ms、英文完全一致 120ms，见 `docs/acceptance/phaseE.md`） |

补充（不属于原计划、但已知的缺口）：
- **颜色/粗细持久化**：现在一次会话内有效，默认 `#ff4040` / 4px。
- **自定义取色器**：现在只有 10 个预设色。
- **结果图到底是重新抓屏还是用冻结帧**：现在是重新抓屏（与预览可能有桌面漂移），
  要严格所见即所得就改成用冻结帧 + 标注。

`EditorWindow` 的处理：**保留**，降级为"二次编辑入口"（`--edit` 与 Phase 2 验收仍依赖它），不要删。

---

## 6. 近期已修的重要问题（避免重复踩）

这些是真被咬过、且容易复发的，改动相关代码时注意：

| 问题 | 结论 |
| --- | --- |
| **layer-shell 假成功** | 必须在 `show()` 之前设 shell（见第 3 节） |
| 结果浮层黑边 | 两处：阴影画在错误坐标系（translate 之前）+ 多层半透明填充叠加导致 alpha 累积成硬黑边。改成非重叠 1px 描边环 |
| 浮层工具超出边框 | 卡片按按钮放宽时用了 `buttons().last().right()` —— 按钮从右往左摆，该值**恒等于卡片宽度**，永远量不出溢出。改为直接算按钮条所需总宽度 |
| REC 秒数不动 | `stats.elapsedMs` 只在 `stop()` 里赋值一次 → 录制期间恒为 0。改为每帧同步 |
| 一次性 CLI 点「保存」后进程不退 | `run()` 里再进一次 `QCoreApplication::exec()` 会**重置 quit 标记**，前一个 quit 被丢弃。改用局部 `QEventLoop` + 先查 `isVisible()` |
| 一次性 CLI 点「编辑」编辑器被带走 | 等浮层期间编辑器才打开，等完必须**重新判断** `m_editorOpened` |
| 复制 GIF 后剪贴板为空 | `wl-copy` 靠 fork 的子进程持有选区；本进程退出→管道关闭→持有者被杀。改用 `QProcess::startDetached` + shell 重定向 |
| 中间文件吃内存 | 本机 **`/tmp` 是 tmpfs**，无损中间文件 ≈554MB/分钟 → 会吃内存。已改落磁盘 `~/.cache/seashot/recording` + 启动清理 + 录制中 <2GB 自动停 |

完整列表见 `docs/known-issues.md`（ISSUE-001 ~ ISSUE-013；013 是贴图的自排除取舍）。

---

## 7. 明天开工速查

```fish
cd ~/V/SeaShot

# 构建并安装（改完代码必做）
cmake --build build && ./install.sh --no-build

# 单元测试（9 套）
cd build && ctest --output-on-failure; cd ..

# 截图主链路验收（改动后必须仍全绿）
bash scripts/acceptance-phase1.sh

# 就地编辑（Phase A1）验收：程序化拖框 → 1:1/手柄/压暗 自动判定 + 落盘证据
bash scripts/acceptance-phaseA1.sh

# 就地编辑（Phase A2）验收：工具条命中/1–9 切换/hover/自动避让 + 工具条 2 倍放大图
bash scripts/acceptance-phaseA2.sh

# 就地编辑（Phase A3）验收：9 种工具落成标注 / 撤销 / 预览==导出 / 「完成」把标注带进结果
bash scripts/acceptance-phaseA3.sh

# 就地编辑（Phase A3.1）验收：颜色/粗细面板（选色、拖滑杆、面板点击语义 + 放大图）
bash scripts/acceptance-phaseA3-color.sh

# 就地编辑（Phase B）验收：拖手柄改选区 / 方向键微调 / Ctrl+A 全选 / Ctrl+C 复制 / Ctrl+S 保存
bash scripts/acceptance-phaseB.sh

# 全局快捷键（Phase F）验收：门户可用性/静默查询/请求不挂死/常驻自检/KGlobalAccel 未回归
bash scripts/acceptance-phaseF.sh

# 贴图（Phase C）验收：拖动/缩放/右键菜单/多张并存/全屏之上仍可见（约 40s，会弹一个全屏白底参照窗）
bash scripts/acceptance-phaseC.sh

# 长截图（Phase D）验收：起一个会自己滚的测试窗口，验对齐/内容/裁列/收工（约 40s）
bash scripts/acceptance-phaseD.sh

# OCR（Phase E）验收：合成中英测试图 → 识别比对 + Ctrl+O 面板/复制（约 20s）
bash scripts/acceptance-phaseE.sh

# 编辑器验收
bash scripts/acceptance-phase2.sh

# 就地编辑探针（若要复验 Phase 0 结论）
./probes/build/inplace-probe /tmp/frozen.png

# 长截图拼接探针（Phase D 算法，若要复验）
cmake -S probes/cmake -B probes/build -G Ninja && cmake --build probes/build
./probes/build/scroll-stitch-probe /tmp/stitch   # 退出码 0 = 四场景全过
```

**提交习惯**（本项目一直这么做）：每个改动一个 commit，信息写清**根因**而不只是"改了 X"，
把实测数字带上；文档（`docs/known-issues.md`）同步更新状态。

---

## 7.5 全局快捷键（Phase F，2026-10-17 加）

用户要求："让应用可以自己注册全局快捷键 让用户自己决定授权还是不授权" →
走 **xdg-desktop-portal `GlobalShortcuts`**：应用发起 → **系统弹授权对话框** → 用户允许/拒绝。
- 入口：设置里的「向系统申请注册（会弹授权对话框）」/ 托盘「注册全局快捷键（由我申请）…」/
  `seashot --hotkey-portal-request`；状态：`seashot --hotkey-portal-status`、`--hotkey-status`（两条路都报）。
- 用户**已实测通过**：授权对话框 → 允许 → `first-run-demo=Ctrl+Alt+A` 落地 → 按键即截图。
- 坑与 KDE 语义（共 5 个坑 + 2 条语义）见 `docs/known-issues.md` ISSUE-014；
  验收见 `docs/acceptance/phaseF.md`（8/8）与 `scripts/acceptance-phaseF.sh`。

## 8. 待用户决策的开放项

0.5 ~~首次运行要不要自动发起 portal 授权~~ → **已做**（2026-10-17）：常驻第一次启动时，
  若"没配置键 + 门户里也没有绑定 + 没问过" → 自动发起一次（于是弹授权对话框）；
  已经授权过则只打一行 `HOTKEY_FIRST_RUN already-authorized`，不打扰。
  验收用 `SEASHOT_NO_FIRST_RUN_ASK=1` 关掉（phase3/phaseF 已加）。
1. **GIF 剪贴板 MIME**：现挂 `image/gif`（聊天/浏览器可当动图粘）。
   若用户实际粘到某程序里变成文件或粘不进去 → 改挂 `text/uri-list`。**取决于用户粘到哪**。
2. **`acceptance-phase4.sh` 的 F 段判据已按新行为修好**（ISSUE-012，已解决）：
   现在验的是"转发=1 + 停止=1 + 延迟导出 + 浮层自动点「导出 GIF」→ GIF 落盘"，
   `acceptance-phase4.sh` **7/7 全绿**。
3. **长截图（Phase D）** 的拼接算法与滚动检测都无现有验证，可能需先做可行性探针，像 Phase 0 那样。
4. **用户提过"问题很多"**，但只明确列了编辑器这一块。
   其余界面问题**尚未列举** —— 明天可主动问一次，按用户给的优先级修，不要靠猜。
