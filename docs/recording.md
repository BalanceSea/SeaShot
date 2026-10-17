# 🎬 录制与导出

本文是 `README.md` 录制相关内容的展开：入口、帧源与回退、帧率与丢帧、中间文件与清理，
以及录完之后怎么导出。

## 入口

```bash
seashot --record                                # 交互选择区域
seashot --record --rect 300,200,320,240         # 直接录指定区域
seashot --record --seconds 10                   # 开始后 10 秒自动停止（0 = 手动停）
seashot --record-stop                           # 让常驻实例停止录制
```

- 全局快捷键也能开录：**录制中按同一个截图快捷键 = 停止录制**（不再是截图）。
- 托盘菜单的「录制 GIF…」与快捷键走同一条 `stopRecording` 路径。

## 录完之后：保存 / 导出窗口

交互式录制（没有 `--save`）结束时会：

1. 先只收尾**无损中间文件**（延迟导出），
2. 自动打开**「保存 / 导出」窗口的「录制」页**，由你选：

| 按钮 | 结果 |
| --- | --- |
| 导出 GIF… | 按默认目录 + 文件名模板导出 `seashot_<时间戳>.gif`，中间文件随后删除 |
| 导出 MP4… | 同上，导出 `.mp4`（H.264 / yuv420p） |
| 复制 GIF | 先导出 GIF，再整体放进剪贴板（动图不能只投一帧） |

需要别的路径时用脚本式调用，按扩展名一次到底、不弹窗口：

```bash
seashot --record --rect 300,200,320,240 --seconds 10 --save clip.gif
seashot --record --rect 300,200,320,240 --seconds 10 --save clip.mp4
```

也可以从托盘菜单 **「保存 / 导出上一次结果…」** 重新打开这个窗口（只要中间文件还在）。

## 帧源：PipeWire 优先，轮询回退

| 路径 | 何时使用 | 备注 |
| --- | --- | --- |
| **PipeWire**（portal ScreenCast） | 编译时启用 `libpipewire`，且有可复用的 portal 授权 token | 真正的视频流，60 fps 场景明显更好 |
| **轮询**（`org.kde.KWin.ScreenShot2`） | 没有 token、没编 libpipewire，或显式要求轮询 | 按帧率请求单帧截图，稳定但开销随分辨率上升 |

- KWin 6.7 没有录制用的 D-Bus 接口（Phase 0 已确认），所以录制只能走上面两条。
- 强制使用轮询排查问题：`SEASHOT_RECORD_POLLING=1 seashot --record`。
- portal 授权**首次会弹对话框**，之后靠 restore token 静默；token 文件默认
  `~/.cache/seashot/screencast.token`（`--portal-token-file` 可指定）。

## 帧率与时间轴

- 帧率默认 **15 fps**（`record/fps`），设置界面里可改；脚本里用 `--record` + 设置控制。
- 定时器用 `QTimer::PreciseTimer` 触发，取帧是异步的：**上一帧还没回来就丢这一帧**
  （宁可丢帧也不排队堆积）；窗口尺寸变化导致的错帧也会被丢弃。
- 丢掉的帧在编码时**复用上一帧补足时间轴**，所以播放时长与真实录制时长一致，
  但缺失的运动细节补不回来。
- MP4（yuv420p）要求偶数宽高：奇数时**向右/向下补 1 像素**
  （`-vf pad=ceil(iw/2)*2:ceil(ih/2)*2`）。

## 中间文件与磁盘

- 位置：`~/.cache/seashot/recording/seashot-record-<pid>.mkv`（**磁盘**，不是 `/tmp`）。
  本机实测 `--rect` 小区域约 14MB；**全屏无损约 554MB/分钟**，所以特意避开了 tmpfs。
- 格式由 `record/format` 决定：`lossless` = FFV1 无损（体积大、转 GIF/MP4 只损失一次）；
  `mp4` = 直接录 H.264（体积小、转 GIF 多一次有损）。
- **启动清理**：每次启动删除"进程已不存在"的残留中间文件（按 PID 判断），不会误伤正在录制的另一实例。
- **空间保护**：录制中每秒检查可用空间，低于 **2 GiB** 立即停止采集
  （无损 554MB/分钟 ≈ 3.6 分钟余量），宁可提前停也不写满磁盘。
- 导出成功后中间文件**立即删除**；导出失败时保留，方便重试。

## 录制指示器

- 贴在录制区域**外面**（上方，放不下则下方）的 layer-shell 小条，显示 `REC`、计时、帧数与「停止」按钮。
- 因为它在本应用的窗口里，Wayland 下会被**自排除**，不会录进自己的 GIF/MP4。
