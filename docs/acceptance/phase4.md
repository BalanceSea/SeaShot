# Phase 4 验收记录（GIF 录制）

- 日期：2026-10-16
- 二进制：`~/.local/bin/seashot`
- 复现：`bash scripts/acceptance-phase4.sh`（原始输出 `docs/acceptance/phase4.log`）
- 结果：**7 项自动化判据全部通过**

> **2026-10-17 更新（ISSUE-012）：F 段判据已重写。**
> 常驻实例的交互式录制现在走「延迟导出」（`setDeferExport`，弹 GIF/MP4 格式选择浮层），
> `RECORD ok path=` 本就是空的，而 F 段当时还要求它非空 → 必失败（**行为变更没同步判据**）。
> 现改为验「转发=1 + 停止=1 + 延迟导出标记 + `SEASHOT_TOAST_AUTOCLICK=gif` 让浮层自动点
> 「导出 GIF」→ `RECORD_EXPORT ok format=gif` + GIF 真的落盘」，仍是端到端判据而不是放宽。
> 顺带修了 G 段：中间文件已从 `/tmp` 改到 `~/.cache/seashot/recording`（ISSUE-011），
> 旧判据等于空跑 —— 现在两个目录都查。详见 `docs/known-issues.md` 的 ISSUE-012。

## 交付内容（对照 PLAN 第 4 节 Phase 4）

| 需求 | 状态 | 实现 |
| --- | --- | --- |
| 只录选定区域 | 完成 | 复用选择覆盖层（`Intent::Record`）：拖动选矩形，或单击窗口录其外框区域 |
| 帧率默认 15、可调 | 完成 | `record/fps`（配置界面已可改）；验收实测 15fps 与 10fps 两种 |
| 无时长 / 体积上限 | 完成 | 帧直接流进 ffmpeg（FFV1 中间文件），不设任何时长/大小闸门；验收录了 8 秒 |
| 停止方式：热键 / 托盘 / 浮层按钮 | 完成 | 全局快捷键在录制中变成"停止"；托盘菜单项切成「停止录制」；指示器上有「停止」按钮 + Enter/Esc |
| 帧采集自己做 + ffmpeg 编码 | 完成 | `GifRecorder` 定时调用 `CaptureBackend::captureRect()` 采帧；`GifEncoder` 两遍 ffmpeg（FFV1 → palettegen/paletteuse） |

新增文件：`src/record/GifEncoder.{h,cpp}`、`src/record/GifRecorder.{h,cpp}`、
`src/record/RecordingIndicator.{h,cpp}`；新 CLI：`--record`、`--record-stop`、`--seconds N`。

## 逐条验收结果

| # | 判据 | 结果 | 证据 |
| --- | --- | --- | --- |
| A | 单测（编码器命令与真实编码、录制器帧率与丢帧策略） | **PASS**：9 个测试套件全过（`tst_gifencoder` 4 例、`tst_gifrecorder` 3 例） | `evidence/p4-unit.txt` |
| B | 端到端 3 秒录制（尺寸/帧率/帧数） | **PASS**：`frames=46 dropped=0 failed=0 elapsedMs=3029 fps=15.19`；GIF `320x240`、`r_frame_rate=15/1`、`nb_frames=46` | `evidence/p4-basic.gif`、`p4-basic.txt` |
| C | 录制指示器落在录制区域之外 | **PASS**：指示器 `y=154`（区域 `regionTop=200`）→ 即使没有自排除的后端也不会被录进去 | `evidence/p4-basic.txt` |
| D | 帧率可调 | **PASS**：改成 10fps → GIF `r_frame_rate=10/1`，`frames=21 elapsedMs=2041 fps=10.29` | `evidence/p4-fps10.gif`、`p4-fps10.txt` |
| E | 无时长/体积上限 | **PASS**：8 秒 → `frames=126 fps=15.23`，847111 字节（3 秒版本 382094 字节） | `evidence/p4-long.gif`、`p4-long.txt` |
| F | 停止路径（常驻实例 + 实例间转发） | **PASS**：`--record --rect` 转发给常驻实例 → `--record-stop` 转发停止 → 常驻实例写出 `seashot_20261016_114424.gif`（`frames=32`） | `evidence/p4-resident.txt` |
| G | FFV1 中间文件清理 | **PASS**：`/tmp` 下没有 `seashot-record-*.mkv` 残留 | `p4-unit.txt` + 脚本 G 段 |

内容正确性抽查：`p4-basic.gif` 帧 0 与帧 20 的 RMSE = 0.28（确实在动），
放大后的帧里能看到真实屏幕内容，且**看不到录制指示器**。

## 实现取舍（和 PLAN 里"portal ScreenCast 优先"的偏差，请知情）

PLAN 的 Phase 4 写的是"优先 portal ScreenCast，其次 x11grab"。实际实现改成了
**定时调用我们自己的截图后端**（Wayland → ScreenShot2，X11 → MIT-SHM），原因：

1. Phase 0 已实测 **KWin 6.7 没有录制 D-Bus 接口**；只剩 portal 一条路。
2. portal 路线要引入 PipeWire 客户端消费（本机 `libpipewire-0.3` 1.6.9 与 gst
   `pipewiresrc` 都在，技术上可行），代价是：流协商 + 帧拷贝 + 每次会话的**用户授权弹窗**，
   而且 PLAN 明确要求"帧采集由 SeaShot 自己完成"。
3. 轮询现有后端：
   - 零新依赖、无授权弹窗；
   - Wayland 与 X11 **一套代码**（复用已经验证过的 captureRect 链路）；
   - 实测帧率误差小（15.19 / 10.29 fps）、丢帧率 0%（320x240 与 256x192 区域）。

代价与已知边界（都写进了 `docs/known-issues.md` 的 ISSUE-006）：
- 帧间隔不严格等间隔（定时器 + 单次截图延迟），高帧率或大区域会更明显；
- 每次截图都是一次 D-Bus 往返，CPU 比 PipeWire 直读流高；
- 需要更高帧率/更大区域时，建议后续加 PipeWire 直读路线（保留当前实现作为回退）。

## 人工项（自动化覆盖不到）

1. 托盘 → 「录制 GIF…」→ 拖动选区 → 指示器出现 → 点「停止」（或按 Enter/Esc）。
2. 录制中按**全局快捷键**：应当停止录制（同一个键在非录制状态是截图）。
3. 单击窗口（而不是拖动）录制该窗口外框区域。
4. 录制指示器的外观（位置/闪烁红点/计时）是否符合预期 —— 这一条和 ISSUE-004（UI 美化）相关。

## 顺带修掉/发现的两个测试问题（都属"测试自身不严谨"）

1. `GifRecorder::start()` 原来无条件进本地事件循环等首帧；若后端回调是**同步**的
   （假后端/未来的同步实现），`loop.quit()` 会在 `exec()` 之前发生 → 白等 15 秒。
   现在先判 `got` 再决定是否进循环。
2. 两个单测依赖了跨次运行的临时目录状态（`fixed.png` 已存在 → 序号从 `_1` 变 `_2`），
   跨秒生成文件名也会让断言随机失败。现在每个用例前清空临时目录、模板用固定值。
