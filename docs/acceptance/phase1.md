# Phase 1 验收记录

- 日期：2026-10-16
- 二进制：`~/.local/bin/seashot`（`./install.sh` 安装）
- 复现命令：`bash scripts/acceptance-phase1.sh`（原始输出：`docs/acceptance/phase1.log`，证据：`docs/acceptance/evidence/`）
- 结果：**9 项通过 / 0 项失败**

## 交付内容（对照 PLAN 第 4 节 Phase 1）

| PLAN 里的 Phase 1 任务 | 状态 | 位置 |
| --- | --- | --- |
| 单实例与托盘骨架 | 完成 | `src/app/Application.cpp`（QLocalSocket 单实例）、`src/app/Tray.cpp`（KStatusNotifierItem） |
| KWin 助手脚本 | 完成 | `resources/seashot-kwin.js` + `src/backend/wayland/KWinBridge.cpp` |
| ScreenShot2 封装（异步 + 读满即停） | 完成 | `src/backend/wayland/ScreenShot2.cpp` |
| 坐标转换层 | 完成 | `src/geometry/ScreenMapper.{h,cpp}` |
| layer-shell 覆盖层 | 完成 | `src/overlay/Overlay.cpp` |
| 悬停虚线框（贴 frameGeometry） | 完成 | 同上（虚线框画在 `frameGeometry`） |
| 单击截窗口 / 拖拽截区域 | 完成 | 同上 + `src/core/SelectionModel.cpp` |
| `wl-copy` 剪贴板 | 完成 | `src/util/Clipboard.cpp` |
| 结果浮层（「编辑」「另存为」） | 完成（编辑按钮为 Phase 2 占位） | `src/app/Toast.cpp` |
| `--selftest` | 完成 | `src/app/SelfTest.cpp` |
| `install.sh` | 完成 | `install.sh` |

## 逐条验收结果

| # | 判据（PLAN） | 结果 | 证据 |
| --- | --- | --- | --- |
| A | `seashot --selftest` 全绿 | **PASS**（9 项检查、0 致命失败） | `evidence/a-selftest.txt` |
| B | 覆盖层可见时截图不含覆盖层 | **PASS**：平均亮度 175.85 vs 175.99（差 0.14；若覆盖层入镜整屏压暗 47%，亮度会掉约 120） | `evidence/b1-baseline.png`、`b2-with-overlay.png`、`b2-overlay.txt` |
| B | 覆盖层窗口被 KWin 标记为「排除出截图」 | **PASS**：`--list-windows` 中 `appId=seashot … excluded=1` | `evidence/b2-overlay.txt` |
| C | 窗口截图与屏幕所见一致（frame 区域） | **PASS**：窗口图 730x558，frame 在图中偏移 (65,53) 与 Phase 0 用品红定位块独立测得的偏移一致；frame 区域 RMSE=**0.020**；负对照（错位 20px）RMSE=0.314 → 指标有区分度 | `evidence/c1-window.png`、`c2-area.png`、`c-analysis.txt` || C | 窗口截图含装饰/阴影 | **PASS**：730x558 = frame 600x428 + 阴影边距（左 65 / 上 53 / 右 65 / 下 77），与 PLAN 决策 2、3 一致 | 同上 |
| D | 剪贴板在 SeaShot 退出后仍可粘贴 | **PASS**：退出前后 `wl-paste --list-types` 均为 `image/png` | `evidence/d.txt` |
| E | 连续 20 次区域截图 | **PASS**：20/20，平均 89ms，最慢 97ms（空闲桌面下未出现陈旧帧） | `evidence/e-*.txt`、`e-*.png` |
| F | X11 后端 selftest（代码级 + XWayland 冒烟） | **PASS** | `evidence/f-x11-selftest.txt` |
| F | X11 后端截图（XWayland） | **PASS**：`CAPTURE ok size=200x150` | `evidence/f-x11-capture.txt`、`f-x11.png` |

`--selftest` 的 9 项：后端选择、desktop 授权属性、输出与缩放、窗口枚举（18 个窗口，含栈序）、
区域截图 32x32、剪贴板可用、剪贴板往返、后端限制说明、配置读取。

## C 段的稳健性（第二次修）

C 段要拿「窗口截图」与「屏幕上同矩形」比像素，前提是**桌面上没有别的窗口压着测试窗口**。
第一版只重试 2 次，你在用桌面时就会偶发 FAIL（实测见过 RMSE 0.39 / 负对照 0.44 —— 
两者差不多大，说明屏幕那片根本不是测试窗口）。现在：

1. 每轮先把这个测试窗口激活到最上层，**最多重试 3 轮**；
2. 若仍不达标，用 **KWin 快照的栈序**判断该矩形当时的最上层窗口：
   - 最上层不是测试窗口 → 判 **SKIP**（环境遮挡，打印当时的最上层 appId），不算失败；
   - 最上层确实是测试窗口 → 判 **FAIL**（说明截图/比对真有问题）。

这样「桌面很忙」不会冒充回归，但真问题依然会红。

## 尚未自动化的项（需你手测）
1. **真实鼠标点击链路**：`seashot --capture` 后用鼠标悬停/单击/拖拽。
   自动化只能覆盖到"覆盖层显示 + 快照 + 命中选择逻辑"（后者有单元测试 `tst_selection`），
   事件投递链路没有做注入测试（本机无 `ydotool` 之类的输入注入工具）。
2. **托盘图标**：KStatusNotifierItem 已创建（日志可见），但托盘外观需你在面板上看一眼。
3. **快捷键**：按 PLAN 决策 4，默认不绑定。要验证请二选一：
   系统设置 → 快捷键 → 添加命令 `seashot --capture`；或给已注册的 `capture` action 绑定按键。

## Phase 1 期间新发现的实现约束（已写进代码注释）

1. **D-Bus 服务名必须按进程区分**：最初用固定的 `org.seashot.SeaShot`，导致常驻实例存在时
   第二个 SeaShot 进程（例如 `--rect` 一次性截图）无法注册服务、整个后端初始化失败
   （验收 B 段第一次就是这么红的）。现在用 `org.seashot.SeaShot.p<pid>`，脚本按 pid 生成。
2. **`excludeFromCapture` 的匹配不能做子串**：早期脚本用 `appId.indexOf("seashot") >= 0`，
   把 appId 为 `seashot-probe-window` 的测试窗口也标成了"排除出截图"，
   而 **KWin 对已排除的窗口会直接取消窗口截图**（`Error.Cancelled: Screenshot got cancelled`）。
   现在改为 `appId == "seashot"` 或 `"seashot."` 前缀匹配。
3. **新窗口要自动带上自排除标记**：进程启动时打标记时覆盖层还不存在，
   于是第一次截图会把覆盖层拍进去。现在 KWin 脚本对匹配的新窗口自动应用标记，
   应用侧在显示覆盖层后再请求一次（双保险）。
4. **XWayland 下普通 `GetImage` 返回 `BadMatch`**（实测 code=8, major=73，root 与子窗口都一样），
   而 **MIT-SHM 的 `ShmGetImage` 正常**（`ffmpeg -f x11grab` 也是这条路）。
   X11 后端现在优先走 MIT-SHM，失败才退回 `GetImage`，并在错误信息里带上 X 错误码。
   真实 X11 会话下两条路都应可用，但**未实测**。
5. **窗口截图会被遮挡/被排除影响**：KWin 对"排除出截图"的窗口取消截图（见 2）；
   另外窗口图只有在该窗口确实被合成时才有内容 —— 验收 C 段因此先用 KWin 脚本把测试窗口
   激活到最上层（等价于真实场景：用户悬停的是屏幕上看得见的窗口）。

## 仍然存在的限制（沿用 PLAN）

- **X11 会话未实测**（本机 `/usr/share/xsessions` 为空）：X11 后端只有单元测试 + XWayland 冒烟；
  XWayland 下只能看到/截取 X11 客户端窗口，且截到的内容基本是空的（XWayland 根窗口不呈现 Wayland 窗口）。
- **分数缩放未实测**：本机单输出 `scale=1`；`ScreenMapper` 有 scale=1.25/1.5/2.0 的合成单测。
- **多输出**：覆盖层按"所有输出并集"做成一个窗口，跨屏拖选还需后续处理。
- 编辑器按钮已就位但只打印 `EDIT_REQUESTED phase2-pending`（Phase 2 交付）。
