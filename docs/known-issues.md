# 已知问题 / 待办（按反馈记录）

> 规则：你反馈的问题先进这里（含复现方式、已试过什么、下一步候选方案），
> 修完再把它挪到 `docs/acceptance/` 对应阶段的记录里并标注修复提交。

## 待修

### ISSUE-008 串流路径（portal ScreenCast + PipeWire）—— 免弹窗已达成，接帧未验证

- **背景**：你问过"录 GIF 能否不要授权弹窗"。Wayland 下客户端拿不到屏幕内容，除非
  用户批准过一次（没有任何客户端能绕过）；能免弹窗的机制是 portal 的 restore token。
- **状态**：**免弹窗已验证可行**（首次点一次即可，之后永久静默）。

**实测证据（2026-10-16）**

首次（勾选「Allow restoring on future sessions」）：
```
PORTAL_SCREENCAST_RESPONSE Start code=0 keys=restore_token,streams startLatencyMs=3590
PORTAL_SCREENCAST_RESULT ... restoreToken=XakNXJWJS6RrhxlXRa7EvQ
权限库: Set "screencast" true "XakNXJWJS6RrhxlXRa7EvQ" "seashot" "yes" "KDE"
  → ~/.local/share/flatpak/db/screencast 时间戳变当天；token 落盘 ~/.cache/seashot/screencast.token
  → portal 日志: "Replacing restore data received from portal impl with a token"
```

第二次（带 token，**无对话框**）：
```
PORTAL_SCREENCAST_BEGIN restoreToken=XakNXJWJS6RrhxlXRa7EvQ
PORTAL_SCREENCAST_RESULT ok=1 usedRestoreToken=1 startLatencyMs=7
PORTAL_SCREENCAST_DIALOG_LIKELY 0 (startLatencyMs=7)
```

**根因（曾长期误判，留档）**：`persist_mode` / `restore_token` 属于 **SelectSources** 的 options，
不是 `CreateSession`。前端 `screen-cast.c` 的 `screen_cast_select_sources_options[]` 只认这两个键，
KDE 也只在 `SelectSources` 里读（`screencast.cpp:206`）；KDE 的 `CreateSession` 对 options 是 `Q_UNUSED`。
放错的后果：KDE 的 `session->persistMode()` 恒为 `NoPersist` → `Start` 里
`if (persistMode() != NoPersist)` 为假 → 不回传 `restore_data` → 前端不
`generate_and_save_restore_token` → 应答里没有 token、权限库零流量、每次必弹窗。
- 附带纠正过一个错误推断："权限库没流量 ⇒ app id 不对"。**前端只在铸 token 时才碰权限库**，
  所以没流量不能推出身份问题。
- 附带做了正确但非根因的改动：私有 D-Bus 连接 + 首个调用即 `Registry.Register("seashot")`
  （`0f12294`）—— 身份确定化，命令行/桌面文件/自启都是同一个 id，值得保留。

**接帧验证：已通过（2026-10-16）** ✅

`--probe-portal-screencast --probe-pipewire-frames N` 实测真的出帧了：

```
PORTAL_SCREENCAST_RESULT ok=1 streams=1 streamsPresent=1 node=88 usedRestoreToken=1 startLatencyMs=7
PIPEWIRE_PROBE_NODEID 88（来自 portal 应答的 streams）
PIPEWIRE_PROBE frames=10 firstFrameMs=301 elapsedMs=1471 bytes=8678871
抽出的第一帧：1920x1080，均值 169，标准差 80 → 是真实屏幕内容
```

两个关键点（都踩过坑）：
1. **必须从 portal 应答的 `streams` 里取 node id**。手工迭代 `a(ua{sv})` 会让 Qt 直接 abort
   （"QDBusArgument: write from a read-only object"）；改用注册元类型 + `qdbus_cast` 才稳。
   顺带：`pw-dump` **看不到**这个节点（它只存在于 portal 给的那条 fd 连接里），
   所以"枚举找节点"这条路是不通的。
2. `pipewiresrc` 要用 **`path=<nodeid>`**（收数字 id），不是 `target-object=<id>`
   （那个要的是节点名/序列号，给 id 会报 target not found）。

**"Stream error: target not found" 的定性（重要）**：不是拆卸噪音，是**真错误** ——
不指定目标时 `pipewiresrc` 会拿 PW_ID_ANY 去连，服务端就报这个。
指定 `path=88` 之后立刻出帧。这解释了此前每一次尝试日志里的那两行。

**已排除的旧说法**：早期记录写"仍未验证/可能建不起来"，现已确认**能出图**。

**下一步（尚未做）**：真正把录制切到 PipeWire 需要引入 GStreamer 或 libpipewire 依赖
（当前应用没有），并重做 GifRecorder 的取帧路径。属于独立任务，需要你决定是否要切。

**仍未验证（下一步的真正风险点）**

- `OpenPipeWireRemote` 拿到了 fd（`fd=22`），但**从未消费过帧**。KDE portal 日志里每次都有：
  ```
  Stream error:  target not found
  PipeWire remote error: -2 target not found
  ```
  无法确定这是"探测程序拿到 fd 就 close 导致的拆卸噪音"，还是"流本身建不起来"。
  **结论：现在还谈不上"录 GIF 改走 PipeWire"** —— 必须先做接帧验证（GStreamer/pipewiresrc
  连 fd + node id，看是否出帧），否则现有 ScreenShot2 轮询实现不动。
- 流参数（node id）目前**故意不解析**：手工迭代 `a(ua{sv})` 会让 Qt 直接 abort
  （`QDBusArgument: write from a read-only object` + SIGABRT，已实测）。接帧时必须用
  `qdbus_cast` / 注册元类型的正规做法。

**工具（可重复验证）**

```fish
bash scripts/portal-debug.sh on|off          # 开关 portal 调试日志（可逆）
bash scripts/probe-n1-permission-store.sh    # 监听权限库 + 抓 portal 日志 + 抓探测输出
seashot --probe-portal-screencast            # 完整流程（首次会弹窗，之后静默）
seashot --probe-portal-screencast --probe-portal-no-start   # dry-run：只建会话，不弹窗
```

## 已修（留档，避免回归）

### ISSUE-004 整体 UI 美化

- **状态**：**已完成**（两版；规格见 `docs/ui-spec.md`）
- **第一版**：新增 `src/ui/Theme.{h,cpp}`（视觉规格常量 + 语义图标表，颜色全部取自 `QPalette`）；
  结果浮层/录制指示器/编辑器工具栏加图标与 hover·pressed 态；配置界面重做布局与间距。
- **第二版（按你的四条答复）**：应用图标换成你给的图（512 内嵌资源，用于托盘/配置界面/窗口图标）；
  编辑器画布加缩放·平移·滚动条（含 3 例单测）；配置界面改「左侧导航 + 右侧页面」。
- **你明确不做的**：浮层假阴影、深色主题复核。

### ISSUE-001 结果浮层拖动卡顿

- **状态**：已修（提交见 `docs/acceptance/issue-001-drag.md`；回归基准 `scripts/benchmark-toast-drag.sh`）
- **反馈**：「拖动截图窗口还是会卡顿」（2026-10-16，第二次反馈）
- **根因（实测）**：浮层靠 layer-shell 的 `set_margin` 定位，而 `LayerShellQt::Window::setMargins()`
  只把新边距放进待提交状态，**需要一次 surface commit 才生效**。拖动时我们只改边距、不产生新缓冲，
  于是 KWin 只在"碰巧有别的提交"时才应用新位置 → 位置以秒级间隔成块跳动。
- **修法**：`setMargins()` 后显式 `update()` 促成提交（卡片有 QPixmap 缓存，代价一次 blit ~70µs）；
  同时去掉 8ms 合并提交定时器（它反而加延迟），改为逐事件提交 + 跳过重复位置。
- **实测**：lagPx 平均 269 → **0**，最大 564 → **0**。
- **第二轮（闪烁 + 不顺畅）**：每次定位提交两次（`setMargins` 的仅状态 commit +
  `update()` 的整块缓冲 commit），240Hz 鼠标下每秒 ~390 次整块重传 + 层重排 → 闪烁。
  改为「只标 1px 脏区（仍会 attach+commit，但不重传内容）」。
  - **第三轮（"发涩掉帧"）**：我随后加的 11ms 节流是新的元凶 —— 240Hz 屏上把位移量化成
    90 步/秒 + 定时器抖动。最终**去掉节流**：逐事件提交 + 仅 1px 脏区，
    240Hz 输入 lagPxMax=1、60Hz 输入 0；帧回调续链方案实测更差（lag 94px、抖动 14.8ms）已回退。
  - **第五轮（"带涩并且抽搐"）**：换结构 —— 表面固定铺满输出、卡片画在表面内偏移位置，
    不再移动 layer-shell 表面（KWin 图层重排 400/s → 0）。过程中修掉两个坑：
    `QWidget::setMask` 会裁剪绘制（应为 `QWindow::setMask`，只映射输入区域）；
    脏区必须覆盖最近 3 次位置（Qt 的 shm 后端有 2~3 个缓冲轮换，否则残留拖影）。
    旧的"移动表面"实现保留为 `SEASHOT_TOAST_MOVING_SURFACE=1` 供 A/B。
  - **第四轮（"无法移动 / 松手瞬移回原位"）**：第三轮去掉节流时漏了同步
    `m_pendingTopLeft`，松手 flush 把窗口拽回按下时的位置；已修，并把"松手落点"
    断言加进回归基准（基准同时改用真实鼠标事件驱动，之前绕过事件路径才漏掉这个 bug）。
  详见 `docs/acceptance/issue-001-drag.md`（含四轮的量化对比）。
- **回归判据**：250px/s 拖动下，显示位置与目标位置偏差 ≤ 8px（≈32ms）。

### ISSUE-007 同一进程的截图请求拍不到自己的 layer-shell 表面（观测记录）

- **状态**：非 bug，但反直觉，留档免得后人（包括我自己）再踩
- **现象**：进程 A 显示浮层（layer-shell）后，**A 自己**调用 ScreenShot2 `CaptureArea`
  得到的帧里没有浮层；**进程 B** 同时抓同一块屏幕却能拍到（自排除已确认关闭、
  KWin 快照里该窗口 `excludedFromCapture=false`）。
- **实测细节**：A 的帧尺寸/内容正常（均值随浮层出现而变化），但浮层品红诊断边框一个像素都没有；
  B 的帧能在预期坐标找到完整品红框（bbox 与浮层尺寸一致）。
- **影响**：
  - 好处：录制（Phase 4 在同进程抓帧）天然不会录到自己的覆盖层/指示器；
  - 影响：**不能**用"同进程抓帧"来自检自己的浮层外观 —— 需要子进程观测
    （ISSUE-001 的基准就是这么做的：`--bench-capture`）。
- **待查**：是否是 KWin 对"请求方自己的 layer-shell 表面"的特例处理（未深入 KWin 源码）。

### ISSUE-002 结果浮层在全屏应用前面曾经不可见

- **状态**：已修（提交 `d0bbfc2`）
- **成因**：普通 `Qt::Tool` 窗口在 Wayland 下压不过全屏窗口。
- **修法**：浮层改用 layer-shell（`LayerOverlay`）+「锚 top|left + 边距」定位。
- **留档原因**：修 ISSUE-001 时别退回普通窗口，否则这个会复发。

### ISSUE-003 被 kill/崩溃后 KWin 助手脚本残留

- **状态**：已修（提交 `d0bbfc2`）
- **成因**：进程被 SIGKILL/崩溃时来不及 `unloadScript`，残留脚本里的"自排除"状态会继续
  把之后所有 SeaShot 窗口标成"排除出截图"，表现是浮层/覆盖层再也截不到。
- **修法**：启动时扫描缓存目录清理死进程脚本 + SIGTERM/SIGINT 优雅退出并卸载。

### ISSUE-005 托盘菜单析构 double free

- **状态**：已修（提交 `525b7d6`，Phase 3）
- **成因**：`Tray` 析构里手动 `delete m_menu`，但 `KStatusNotifierItem` 已接管 contextMenu
  所有权 → double free。生产影响：配置界面里关掉「常驻托盘」就会崩。
- **修法**：不手动删菜单，交给 item 管理；由新增的 `tst_tray` 单测兜住。
