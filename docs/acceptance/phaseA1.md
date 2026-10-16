# Phase A1 验收记录（就地编辑骨架：覆盖层进入 Annotating 阶段）

- 日期：2026-10-17
- 二进制：`~/.local/bin/seashot`（`./install.sh --no-build`）
- 复现：`bash scripts/acceptance-phaseA1.sh`（原始输出 `docs/acceptance/phaseA1.log`，
  证据 `docs/acceptance/evidence/a1-inplace-*.png`）
- 结果：**6 项自动化判据全部通过**

## 交付内容（对照 `docs/plan-inplace-editor.md` 第 3 节 Phase A 的第 1 条）

`Overlay` 增加 `Annotating` 阶段。交互式截图（热键 / 托盘 / `--capture`）选完区域后
**不再退出覆盖层、也不再打开任何新窗口**，而是在同一个 layer-shell 表面上：

1. 抓一帧整块虚拟桌面（抓的时候覆盖层自己不入镜 —— KWin 自排除，`appId=seashot`）；
2. 把这一帧按**逻辑像素 1:1** 画到覆盖层上（没有任何缩放）；
3. 选区之外压暗、选区画边框 + 8 个手柄；
4. `Enter` / 双击 = 确认（继续走原有截图链路：剪贴板 / 保存 / 结果浮层 / 编辑器）；
   `Esc` / 右键 = 取消。

`SelectionResult` 不变，**标注坐标与选区都仍是逻辑坐标**，所以后面"A2 工具条""B 拖手柄改选区"
都只是在这个固定选区上做增量，不需要改数据模型。

## 自动化判据结果

| # | 判据 | 结果 | 证据 |
| --- | --- | --- | --- |
| 1 | 选完进入 Annotating 阶段（不再直接截图） | **PASS**：`annotating=1 stage=Annotating`，选区 `220,180 540x380` | `evidence/a1-inplace.txt` |
| 2 | **不弹新窗口** | **PASS**：可见顶层部件只有 `seashot-overlay`（没有 `EditorWindow`） | 同上 |
| 3 | 冻结屏幕 **1:1**（无缩放） | **PASS**：冻结图 `1920x1080` == 覆盖层表面 `1920x1080`（`scale=1`） | 同上 |
| 4 | 选区内部**逐像素**等于冻结图 | **PASS**：内缩 4px 逐像素比对，不一致 **0 像素**，最大通道差 **0** | 同上 |
| 5 | 选区边框 + 8 手柄 + 区外压暗 | **PASS**：8 个手柄中心全部命中强调色 `#3daeff`（8/8）；区外 4 个探针都符合 `×0.529`（`kDimAlpha=120`） | 同上 |
| 6 | 视觉证据落盘 | **PASS**：`evidence/a1-inplace-overlay.png`（1920x1080）与 `-frozen.png` | 两张 PNG |

第 4 条是这一阶段最关键的判据：它把"1:1、无缩放、与选区像素对齐"从"看起来对"
变成"逐像素为 0 差异"。做法就是把抓到的冻结图和覆盖层实际画面在同一坐标下逐像素比。

## 回归（改动动了截图主链路，必须守住）

| 套件 | 结果 |
| --- | --- |
| `ctest`（9 套单测） | **9/9 通过** |
| `scripts/acceptance-phase1.sh` | **9 通过 / 0 失败 / 0 跳过**（含 B 段覆盖层自排除 RMSE=0、E 段连续 20 次截图 20/20） |
| `scripts/acceptance-phase2.sh` | **4 通过 / 0 失败** |
| `scripts/acceptance-phase3.sh` | **7 通过 / 0 失败 / 0 跳过** |

**脚本式调用行为不变**：`--rect` / `--window` / `--save` / `--capture-screen` / `--overlay-hold`
都不经过 `startCapture()` 的标注开关，走的还是原来那条"选完直接截图"的路径
（这正是 Phase 1 验收 A/C/E 段仍然全绿的原因）。

## 与计划的偏差（需要你知情）

- **A1 阶段的出口键是 `Enter`（或双击）**：因为工具条要到 A2 才有，A1 期间交互式截图
  选完区域后**需要按一下回车**才会出结果浮层。这符合"每步都给一个可验证的中间态"的分步原则，
  但如果你觉得这一下回车很别扭，可以先跳到 A2（工具条上会有「完成」按钮）。
- `SEASHOT_INPLACE_ANNOTATE=0 seashot --capture` 可临时退回"选完直接截图"的旧行为（A/B 对比/应急）。
- 冻结图**整屏**画上去，所以标注期间屏幕内容会"静止"（这是刻意的：标注时有东西在动会漂）。
