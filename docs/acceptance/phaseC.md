# Phase C 验收记录（贴图 / 钉图）

- 日期：2026-10-17
- 二进制：`~/.local/bin/seashot`（`./install.sh --no-build`）
- 复现：`bash scripts/acceptance-phaseC.sh`（原始输出 `docs/acceptance/phaseC.log`、
  `evidence/c-pin.txt`、`evidence/c-pin-inplace.txt`，证据 `docs/acceptance/evidence/c-pin-*.png`）
- 结果：**10 项自动化判据全部通过**

## 交付内容（对应用户诉求 ⑥「贴图/钉图到屏幕」）

新类 `src/app/PinWindow.{h,cpp}`：把一张图钉在屏幕上的一块浮窗里。

| 能力 | 行为 |
| --- | --- |
| 显示 | layer-shell `LayerOverlay` 层，**1:1** 显示（滚轮缩放后按 zoom 缩放），带深/浅两层描边 |
| 拖动 | 按住图内拖动（`m_offset` 在固定表面内移动，**不动表面本身** —— ISSUE-001 的教训） |
| 缩放 | 滚轮 0.2×–5×，**以光标为锚点**（光标下的那个像素不动） |
| 右键菜单 | 自绘三项：**复制**（剪贴板拿到 `image/png`）/ **保存到默认目录** / **关闭** |
| 键盘 | `Esc`：菜单开着先收菜单，否则关掉这张贴图（需要先点一下贴图拿到焦点） |
| 多张并存 | 每张一个 `PinWindow`；关掉一张不影响其它张 |
| 输入穿透 | `QWindow::setMask` 只把「贴图 ∪ 菜单」设为输入区，**贴图以外点击穿透到桌面** |
| 入口 | ① 就地标注时按 `Ctrl+T`（工具条上也有一个 📌 格）→ 把标注后的结果钉到屏幕上；<br>② 命令行 `seashot --pin <png>` |

## 自动化判据结果

| # | 判据 | 结果 | 证据 |
| --- | --- | --- | --- |
| 1 | 贴图窗口用 layer-shell 建起来了 | **PASS**：`贴图 240x160 zoom=1.000 矩形=300,260 240x160 layerShell=1` | `evidence/c-pin.txt` |
| 2 | 拖动 + 滚轮缩放 | **PASS**：拖动 `(+70,+50)` 后矩形 `395,333 264x176`；滚轮后 `zoom=1.1` | 同上 |
| 3 | **贴图真的合成到屏幕上** | **PASS**：另一个进程截屏后，在贴图矩形内采到贴图 1 的 `#ff2d55`、贴图 2 的 `#2dff88` | `evidence/c-pin-screen.png` |
| 4 | **全屏应用前面仍然可见** | **PASS**：全屏白底参照窗口铺满后（左上角采样 `255 255 255`），两个贴图位置**仍采到自己的颜色** | `evidence/c-pin-on-fullscreen.png` |
| 5 | 右键菜单「复制」 | **PASS**：`PIN_COPY ok mime=image/png`；另一进程 `wl-paste --list-types` 也看到 `image/png` | `evidence/c-pin.txt` |
| 6 | 右键菜单「保存」 | **PASS**：`PIN_SAVE ok path=…/seashot_*.png`（240x160），验完已删除 | 同上 |
| 7 | 多张并存 + 关闭无残留 | **PASS**：两张并存 → 关一张还剩 1 张 → 全关后进程内为空，窗口列表里也没有 `appId=seashot` 的窗口 | 同上 |
| 8 | 汇总判定（贴图演示） | **PASS**：`PIN_DEMO PASS`（退出码 0） | 同上 |
| 9 | 就地编辑 `Ctrl+T` = 贴图 | **PASS**：`贴图 540x380 … layerShell=1` —— 标注后的结果被钉到屏幕上 | `evidence/c-pin-inplace.txt` |
| 10 | 视觉证据落盘 | **PASS**：`c-pin-screen.png`（贴图）与 `c-pin-on-fullscreen.png`（全屏参照之上） | 两张 PNG |

第 3、4 条是这一步最关键的判据，做法上有个坑值得记：

> **不能在本进程里截屏核对贴图。** 两个原因叠加：
> ① 同一进程的 ScreenShot2 拍不到自己的 layer-shell 表面（ISSUE-007）；
> ② 贴图也是 `appId=seashot` 的窗口，会被本项目的"自排除"一起排除出**任何**进程的截图。
> 所以验收脚本第 A~D 段**故意**给「贴图进程 + 观察者进程」都加
> `SEASHOT_DISABLE_SELF_EXCLUSION=1` —— 关掉自排除后截到的才是**人眼看到的样子**
> （自排除本身是另一条独立特性，Phase 1 验收 B 段覆盖它）。

## 一个有意保留的取舍：贴图默认不出现在后续截图里

实测（KWin 6.7）：layer-shell 表面报给 KWin 的 `resourceClass` 就是应用的 appId（`seashot`），
**即使给贴图单独设了 layer-shell scope（`seashot-pin`）也看不出来**（诊断脚本读到的仍是
`cls=seashot name=seashot`）。因此贴图和覆盖层/结果浮层一样，属于「本应用的 UI」，
被自排除出截图 —— 也就是说：

- **肉眼看得到**贴图；
- **截图（任何进程）拍不到**贴图。

这是**有意的取舍**（和覆盖层/结果浮层保持一致，也避免"当参考图用的贴图被烘进新截图"）。
若你希望它像 QQ 的钉图那样"贴图也算桌面内容、后续截图能看到"，需要 KWin 侧能按
namespace / window role 区分单个表面 —— 目前做不到；退路是给 `--pin` 进程单独关掉自排除
（会让 `--pin` 与"就地编辑里 Ctrl+T"两条入口行为不一致，所以我没这么干）。

## 回归

| 套件 | 结果 |
| --- | --- |
| `ctest`（9 套单测，含新增的 `translateAll` 用例） | **9/9 通过** |
| `scripts/acceptance-phaseA1.sh` | **6 通过 / 0 失败** |
| `scripts/acceptance-phaseA2.sh` | **10 通过 / 0 失败** |
| `scripts/acceptance-phaseA3.sh` | **13 通过 / 0 失败** |
| `scripts/acceptance-phaseA3-color.sh` | **7 通过 / 0 失败** |
| `scripts/acceptance-phaseB.sh` | **11 通过 / 0 失败** |
| `scripts/acceptance-phase1.sh` | **9 通过 / 0 失败 / 0 跳过**（脚本式调用行为不变） |
| `scripts/acceptance-phase2.sh` | **4 通过 / 0 失败** |
| `scripts/acceptance-phase3.sh` | **7 通过 / 0 失败 / 0 跳过** |
| `scripts/acceptance-phase4.sh` | **7 通过 / 0 失败 / 0 跳过** |

## 还没做 / 下一步

- **A4**：工具条上的「复制 / 保存」按钮（管道已在 Phase B 打通，只剩加两格）。
- **Phase D 长截图**（风险最高，建议先做拼接可行性探针）、**Phase E OCR**（`chi_sim`/`eng` 已就绪）。
- 贴图的持久化（进程退出即消失；若你要"贴图活过 SeaShot 退出"，需要另想办法）。
- 贴图没有右键菜单以外的快捷键（如 `Ctrl+C` 复制当前贴图 —— 需要键盘焦点，暂未做）。
