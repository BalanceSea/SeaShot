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

## 第二轮（反馈"会闪烁并且拖动不顺畅"）—— 提交风暴

第一版的修法（每次定位都 `update()` 促成提交）确实让位置跟上了，但引入了新问题：
**每次定位提交两次**——`setMargins()` 自己发一次"只带状态"的 commit，`update()` 又发一次
"带缓冲"的 commit，而后者会把整张卡片的缓冲重传一遍。

量化（`SEASHOT_BENCH_STEP_MS=4` 模拟 240Hz 鼠标）：

| 变体 | 每秒提交 | 每秒能服务的输入事件 | lagPxMax |
| --- | --- | --- | --- |
| 每次整块重绘（第一版） | 389 | 241 | 1 |
| 只标 1px 脏区 | 420 | 420 | 8 |
| **只标 1px 脏区 + 90Hz 节流（最终）** | **100** | 250（节流后按 90Hz 提交） | **0** |

闪烁的来源就是"每秒几百次整块缓冲提交 + 几百次层重排"：Qt 的 shm 后端每次 `update()`
都要重写并重传整块缓冲，KWin 每次 margin 变化都要重排该层。

最终修法：

1. `applyPosition()` 里对 layer-shell 只调 `update(QRect(0, 0, 1, 1))`：
   Qt 的 shm 后端只拷贝脏区，但**依旧 attach + commit** —— 于是"位置随提交生效"而
   "内容不重传"（拖动时卡片内容本来就不变，视觉无差）。
2. 位置提交节流到 ~90Hz（`m_moveThrottle`，11ms）：240Hz 鼠标下把层重排从 400+/s
   降到 100/s。松手时强制 flush 最后一次位置，不会丢落点。
3. 保留诊断开关 `SEASHOT_TOAST_FULL_REPAINT_ON_MOVE=1`（强制整块重绘）用于以后 A/B。

最终实测：

```
240Hz 输入：samples=11 lagPxAvg=-0.09 lagPxMax=0   commits=250/2.5s = 100/s
 60Hz 输入：samples=12 lagPxAvg=0.08  lagPxMax=1
回归基准（scripts/benchmark-toast-drag.sh）：PASS（判据 ≤ 8px）
```

CPU（同一台机器、2.5 秒基准）：user 532ms / sys 40ms（含基准自身的 240Hz 定时器与
子进程观测开销，只能当量级参考）。

## 第三轮（反馈"发涩掉帧"）—— 节流才是罪魁

第二轮我加了 11ms（~90Hz）的位置提交节流。240Hz 屏上这就把位移**量化到 90 步/秒**，
再加上定时器抖动，手感就是"发涩、掉帧"（不是闪，是台阶感）。

中间还试过一条"跟着合成器帧回调提交"的路（Qt6 已删 `QWindow::frameSwapped`，
改用 `QWindow::requestUpdate()` + `QEvent::UpdateRequest` 自己续链）。实测**更差**并已回退：

| 变体 | lagPxMax | 提交间隔平均 | 间隔抖动(stddev) | 最大间隔 |
| --- | --- | --- | --- | --- |
| 11ms 节流（用户反馈"发涩"的那版） | 1 | 11ms（被量化） | — | — |
| 帧回调续链（试过，已回退） | **94** | 5.07ms | **14.8ms** | **386ms** |
| **逐事件提交 + 1px 脏区（最终）** | **1**（240Hz 输入）/ **0**（60Hz） | 4.5ms（240Hz）/ 17.5ms（60Hz） | 4.5ms / 7.1ms | 59ms（基准自身子进程开销） |

最终方案就是**不节流**：每个鼠标事件都提交，但每次只改边距 + 标 1px 脏区。
提交节奏自然跟随输入（240Hz 鼠标就是 240 次/秒，每次开销 ~70µs、1px 数据），
既没有"整块重传"的闪烁，也没有"定时器量化"的台阶感。

CPU（同一台机器、2.5 秒基准、240Hz 模拟）：user 532ms / sys 40ms（含基准自身的
240Hz 定时器与子进程观测开销，只能当量级参考）。

### 如果还觉得不够顺

剩下唯一的嫌疑是 **KWin 每次边距变化都要重排该层**（这是协议决定的，客户端绕不开）。
真到那一步的候选方案（按性价比排序，已记在 docs/known-issues.md）：

1. **拖动期间把表面缩成小把手**（例如 220x48 的"拖动中"条），落点后再展开成完整卡片：
   表面像素量从 MB 级降到 KB 级，KWin 每帧重排的代价下降两个数量级；
2. 拖动期间改用普通窗口 + `startSystemMove()`（合成器自己搬窗口，最顺）；
   代价是这段时间浮层会落到全屏应用之后（ISSUE-002 的老问题），且普通窗口在 Wayland
   上无法由客户端定位 —— 只能作为"拖动中"的临时替身。
