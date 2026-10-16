# ISSUE-001 修复记录：结果浮层拖动卡顿

- 日期：2026-10-16
- 复现：`bash scripts/benchmark-toast-drag.sh 3000`（基准脚本；证据 `evidence/issue001-drag-bench.txt`）
- 结论：**已修**。拖动跟随误差从 **269px（最大 564px）** 降到 **0px**

## 根因（实测出来的，不是猜的）

浮层是 layer-shell 表面，位置靠 `zwlr_layer_surface_v1.set_margin` 决定。
`LayerShellQt::Window::setMargins()` 只是把新边距写进待提交状态，
**真正生效需要一次 surface commit**。我们拖动时只改边距、不产生新缓冲，
于是 KWin 只在"碰巧有别的提交发生"时才应用新位置 —— 表现就是位置以**秒级间隔成块跳动**，
人眼看到的就是"拖动跟不上、一顿一顿"。

测量数据直接暴露了这个模式（修复前，同一轮基准）：

```
DRAGBENCH_SAMPLE step=82  observed=304 targetAtFrame=556 lagPx=252 lagMs=1008
DRAGBENCH_SAMPLE step=100 observed=304 targetAtFrame=628 lagPx=324 lagMs=1296   ← 位置卡住不动
DRAGBENCH_SAMPLE step=120 observed=304 targetAtFrame=704 lagPx=400 lagMs=1600
DRAGBENCH_SAMPLE step=141 observed=784 targetAtFrame=788 lagPx=4   lagMs=16     ← 偶尔跳一次
DRAGBENCH_SAMPLE step=281 observed=784 targetAtFrame=1348 lagPx=564 lagMs=2256
DRAGBENCH samples=13 lagPxAvg=269.5 lagPxMax=564 lagMsAvg=1078
```

## 修法（两处，都很小）

1. **改完边距后显式促成一次提交**：`Toast::applyPosition()` 里 `setMargins()` 之后调用
   `update()`（卡片内容有 `QPixmap` 缓存，代价就是一次 blit，实测单次 ~70µs）。
2. **去掉 8ms 合并提交定时器**：它反而凭空加了最多 8ms 延迟；现在逐事件直接提交，
   只跳过"位置没变"的重复事件（`m_lastAppliedTopLeft`）。

## 修复后

```
DRAGBENCH_SAMPLE step=39  observed=400 targetAtFrame=400 lagPx=0 lagMs=0 applyUs=85
DRAGBENCH_SAMPLE step=118 observed=716 targetAtFrame=716 lagPx=0 lagMs=0 applyUs=71
DRAGBENCH_SAMPLE step=218 observed=1116 targetAtFrame=1116 lagPx=0 lagMs=0 applyUs=110
DRAGBENCH samples=10 lagPxAvg=0 lagPxMax=0 lagMsAvg=0
```

| 指标 | 修复前 | 修复后 |
| --- | --- | --- |
| lagPx 平均 | 269 | **0** |
| lagPx 最大 | 564 | **0** |
| 单次定位开销 | ~30µs | ~70µs（多了一次缓存 blit） |

## 验收判据（已写进 `scripts/benchmark-toast-drag.sh`）

在 250px/s 匀速拖动下，**显示位置与目标位置的偏差 ≤ 8px**（≈32ms，两帧以内）。
脚本把这条当 PASS/FAIL，可作为后续回归门。

## 测量方法（为什么会绕一圈）

一开始想在**同一进程**里抓帧观测——结果抓不到自己的浮层，且确认过不是自排除导致
（`excluded=0`、另一个进程能拍到）。这条独立发现记在
`docs/known-issues.md` 的 ISSUE-007。所以基准改成：

1. 基准进程按 4px/16ms 拖动浮层，并把 (时间, 目标 x) 轨迹记在内存里；
2. 每 320ms 起一个子进程 `seashot --bench-capture <png>`（带
   `SEASHOT_DISABLE_SELF_EXCLUSION=1`）抓全屏，打印**帧时间戳**；
3. 从 PNG 里找浮层的品红诊断边框 → 得到实见 x；
4. 用帧时间戳在轨迹里反查"当时的目标位置"，两者相减 = 滞后。

诊断边框是 `Toast::setDragMarkerEnabled(true)` 画的（把正常描边换成 4px 纯品红，
便于像素级定位）；`--toast-drag-bench` / `--bench-capture` 两个 CLI 都标注为诊断用途。

## 残留风险（留档）

- 逐事件提交意味着高刷鼠标（如 240Hz）下每秒最多 240 次 commit + blit。
  实测提交开销 ~70µs，理论上没问题；如果你在 240Hz 下感到新的问题，可以退回到
  4ms 合并（把 `applyPosition` 调用放进一个 4ms 定时器即可）。
- 这个修法是"客户端多干一点活保证提交"，不依赖 KWin 版本行为；但若将来 KWin 改变
  layer-shell 边距的应用时机，需要重跑本基准确认。
