# SeaShot Phase 0 实测结论（能力探测，未写应用代码）

本文只记录**实测**结果。每条结论都给出所用命令；原始输出留在 `probes/out/`，
可直接复核。判定分三档：**已验证 / 未验证 / 不可行**。

探测脚本都在 `probes/`（`*.sh` 可重复执行，`*.cpp` 为探测程序）。

---

## 0. 环境基线

| 项 | 实测值 | 来源 |
| --- | --- | --- |
| 发行版 / 桌面 | Arch Linux，KDE Plasma 6，`XDG_SESSION_TYPE=wayland` | `env`；`/etc/os-release` |
| 合成器 | KWin **6.7.5**，Qt **6.12.0**，Wayland 模式 | `qdbus6 org.kde.KWin /KWin org.kde.KWin.supportInformation`（`probes/out/kwin-support-info.txt`） |
| X11 | XWayland `DISPLAY=:1`；`/usr/share/xsessions` **为空**（无 X11 会话可登录） | `ls /usr/share/xsessions` |
| 输出 | 单输出 `DP-3`，1920x1080，**Scale: 1**，当前 239.96Hz | `kscreen-doctor -o`（`probes/out/kscreen-outputs.txt`）、supportInformation"Screen 0 / Scale: 1" |
| 装饰 | `org.kde.breeze`，**borderSize: 0**，`gridUnit: 10` | supportInformation「Decoration」段 |
| 截图接口 | `org.kde.KWin.ScreenShot2`，**Version = 5** | `qdbus6 … Properties.Get … Version` |
| 网络 | 直连 GitHub/KDE 均 TLS 失败；`127.0.0.1:7897` 有监听但代理不通；`baidu`/`tuna` 直连 200 | `curl -sS -m 8` |

> 说明：`NextKde/kwin/window-bridge.js` 注释里记的 `QRect(0,0,1696,1200)` 与本次实测
> 不符（现为 1920x1080 / scale 1），实现时**不要**沿用那个数字。

---

## ① KWin 截图接口 —— **已验证**

```
qdbus6 org.kde.KWin /org/kde/KWin/ScreenShot2
```
实测该对象存在，`org.kde.KWin.ScreenShot2` 提供方法（原始 XML：`probes/out/screenshot2-introspect.xml`）：

| 方法 | 签名 |
| --- | --- |
| `CaptureWindow` | `s a{sv} h → a{sv}` |
| `CaptureActiveWindow` | `a{sv} h → a{sv}` |
| `CaptureArea` | `i i u u a{sv} h → a{sv}` |
| `CaptureScreen` | `s a{sv} h → a{sv}` |
| `CaptureActiveScreen` | `a{sv} h → a{sv}` |
| `CaptureWorkspace` | `a{sv} h → a{sv}` |
| `CaptureInteractive` | `u a{sv} h → a{sv}` |

返回的 `a{sv}` 实测字段（`probes/out/01-screen.png` 那次调用）：

```
format = 6 (uint)      # QImage::Format_ARGB32_Premultiplied
height = 1080 (uint)   stride = 7680 (uint) = width*4
scale  = 1 (double)    width = 1920 (uint)
type   = raw (QString) screen = DP-3 (QString)      # CaptureWindow 时给 windowId
```

调用成功（`shot2probe screen DP-3`）：1920x1080 PNG，8294400 字节像素。

### 授权规则 —— **已验证**（这是本机最大的坑）

```
./shot2probe area 0 0 64 64 out.png
== ERROR name=org.kde.KWin.ScreenShot2.Error.NoAuthorized
   message=The process is not authorized to take a screenshot
```

KWin 的判定链（由 `screenshot.so` 符号与字符串、以及实测组合得出）：
调用方 PID → `/proc/<pid>/exe` → `KApplicationTrader::query()` 找 **.desktop 文件**
→ 读属性 `X-KDE-DBUS-Restricted-Interfaces`（须含 `org.kde.KWin.ScreenShot2`）。

证据：
- `nm -DC /usr/lib/qt6/plugins/kwin/plugins/screenshot.so` → `KApplicationTrader::query`、
  `QDBusConnectionInterface::servicePid`、`executablePathFromPid`；
- `strings` → `KWIN_SCREENSHOT_NO_PERMISSION_CHECKS`、`Could not find the desktop file for`、
  `Interfaces found for`；
- 全机仅三个 desktop 文件带该属性：`org.kde.spectacle.desktop`、`org.kde.plasmashell.desktop`、
  `org.freedesktop.impl.portal.desktop.kde.desktop`，值均为
  `X-KDE-DBUS-Restricted-Interfaces=org.kde.KWin.ScreenShot2`。

安装下面这个最小 desktop 文件（`kbuildsycoca6 --noincremental` 刷新）后，同样的调用返回像素：

```ini
[Desktop Entry]
Type=Application
Name=SeaShot Probe (Phase 0)
Exec=/home/balancesea/.local/bin/seashot-probe0
NoDisplay=true
X-KDE-DBUS-Restricted-Interfaces=org.kde.KWin.ScreenShot2
```

- `Exec` 必须与进程的 `/proc/self/exe` 一致；探测期间用"含空格的仓库路径"注册时未通过，
  改成无空格路径后通过（**注**：该次失败也可能是 sycoca 尚未生效，两者未完全隔离；
  实现时按"无空格路径 + Exec 与真实 exe 完全一致 + 安装后刷新 sycoca"三条一起满足最稳）。
- 未授权的失败是**立即返回错误**（几十毫秒），不是挂起 —— 可用于启动自检。

### 两条硬性实现约束 —— **已验证**

1. **必须先读管道再等响应（或异步并发）**：KWin 把像素写进我们传入的 `h`(pipe)，
   响应在写完后才回。若用阻塞 `call()` 再读管道，1920x1080（8MB）会撑满管道缓冲区
   → KWin 阻塞在 `write`、调用方阻塞在等响应 → **死锁**（探测中复现）。
   `probes/shot2probe.cpp` 用「`asyncCall` + `poll()` 边读边等」解决。
2. **不能等 EOF**：KWin 取图后不关闭它那份写端副本。实测"等 EOF"版本空等到 60s 超时才返回
   （`== timing: total=61s`）。正确做法：用响应里的 `stride*height` 算精确字节数，**读满即停**。
   修正后同一区域调用 `replyAt=4ms total=7ms`、`replyAt=53ms total=53ms`。

---

## ② KWin 脚本与窗口枚举 —— **已验证**

```
qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.loadScript <abs.js> <pluginName>
qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.start
qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.isScriptLoaded <pluginName>
qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.unloadScript <pluginName>
```

- 加载/启动/卸载全部可用；`print()` 输出进 `journalctl --user -u plasma-kwin_wayland.service`
  （`probes/kwin-probe.js`，原始输出 `probes/out/kwin-windows.txt`）。
- 每个窗口可拿到：`internalId`（带花括号的 UUID，形如 `{35d77d6e-…}`）、`pid`、`resourceClass`、
  `caption`、`active`、`normalWindow`、`frameGeometry`、`clientGeometry`、`excludeFromCapture`
  （可读**可写**）。
- `internalId` 正是 `CaptureWindow(handle)` 需要的 handle（实测用花括号形式可用）。
- **几何可能是小数**：实测 `frame={"x":536.582963768418,"y":227.62036761478473,…}`。
  坐标转换层必须定义唯一的取整/对齐策略，不能假设整数。
- 装饰几何：实测测试窗口 `frame=(660,326,600,428)` / `client=(660,354,600,400)`
  → `frameGeometry` **包含** 28px 标题栏，宽度相同（本机边框 0）。
- `expandedGeometry` **不存在**（`w.expandedGeometry` → `Cannot read property 'x' of undefined`）。

---

## ③ 覆盖层与自排除 —— **已验证**（含一条重要反例）

覆盖层技术可行性（`probes/overlayprobe.cpp`，CMake `find_package(LayerShellQt)` 通过）：

```
SEASHOT_OVERLAY role=applied layer=overlay anchors=all
SEASHOT_OVERLAY shown geometry=0,0 1920x1080 screens=1 dpr=1.000 role=1
```

- LayerShellQt 头文件/CMake/运行时齐备，可拿到 role、置于 `LayerOverlay`、四边锚定、全屏、
  设置 `KeyboardInteractivityExclusive`。
- 覆盖层会出现在 KWin 窗口列表里，`resourceClass` = 程序名（探测程序为 `overlayprobe`；
  注意 Qt 的 app_id 命名要显式设置 `applicationName`/`desktopFileName` 才可控）。

自排除的三种做法与实测结果：

| 做法 | 实测 | 判定 |
| --- | --- | --- |
| 什么都不做 | `CaptureArea` 覆盖层可见时 100% 品红（`60-overlay-visible.png`） | 覆盖层**会**被拍到 |
| 隐藏覆盖层后立即截图 | 发信号隐藏后 16ms 完成截图，结果与"可见时"**字节相同**（md5 一致：`60`/`61`） | **不可靠**：拿到的是含覆盖层的陈旧帧 |
| KWin 脚本设 `overlay.excludeFromCapture = true` | 设置后同一区域截图 meanRGB 由 `255,0,255` 变为 `133,133,74`（底层窗口内容），复位后又可关掉 | **有效**，推荐 |

```js
// 实测可行：KWin 侧给覆盖层开自排除
w.excludeFromCapture = true;   // SEASHOT_EFC uuid={70d17acf-…} before=false after=true err=
```

补充：**窗口截图不需要自排除** —— 覆盖层铺满全屏时 `CaptureWindow(测试窗口)` 的结果与
无覆盖层时**逐字节相同**（`RMSE = 0`），说明窗口截图只渲染目标窗口本身。

另一条相关现象（未完全定因，实现必须防御）：桌面空闲时截图可能返回**上一帧**。
除了上面的 md5 相同，还观察到"隐藏后 1s"的截图内容仍是覆盖层消失后的过渡态。
因此区域截图不能假设"隐藏完就等于干净"，需要有等待/重试策略（见 PLAN）。

---

## ④ 输出与缩放 —— **已验证（scale=1）；scale≠1 未验证**

- 单输出 1920x1080、scale 1：`kscreen-doctor -o`、supportInformation 的 `Screen 0 … Scale: 1`、
  ScreenShot2 返回的 `scale = 1`，三处一致。
- `CaptureScreen` 返回 1920x1080，`CaptureArea` 返回请求的宽高（无裁剪）。
- **未验证**：分数缩放（scale ≠ 1）与多输出。本机只有一个输出且当前 scale=1，
  要真机验证需你同意临时改显示缩放（本 Phase 未擅自修改）。

---

## ⑤ 全局快捷键 —— **已验证可用，但与你的默认值冲突**

- 会话总线上有 `org.kde.kglobalaccel`；本机 `plasma-kglobalaccel.service` 是 `inactive dead`
  → 该名字由 `kwin_wayland` 自己提供。开发资源齐备：`/usr/lib/cmake/KF6GlobalAccel`、
  `/usr/include/KF6/KGlobalAccel/kglobalaccel.h`。
- **`Ctrl+Alt+A` 已被占用**：

```
~/.config/kglobalshortcutsrc
[services][org.kde.spectacle.desktop]
RecordRegion=Meta+Shift+R
_launch=Ctrl+Alt+A          ← 你现有的 Spectacle 启动快捷键
```

→ 见 PLAN.md「待你裁决」第 1 条。

---

## ⑥ 剪贴板 —— **已验证**

```
wl-copy < probes/out/07-area-8.png
wl-paste --list-types      → image/png
wl-paste > roundtrip.png   → cmp: 字节一致
（调用方进程结束后）wl-paste --list-types → image/png
```

在 KWin Wayland 下 `wl-copy` 可用，图像原样往返，且由 `wl-copy` 守护进程持有剪贴板 →
**SeaShot 进程退出后仍可粘贴**，满足"剪贴板持久性"要求。

---

## ⑦ 录制路径 —— 部分已验证

| 候选 | 实测 | 判定 |
| --- | --- | --- |
| KWin 自带录制 D-Bus 接口 | `qdbus6 org.kde.KWin` 对象列表里没有任何 Recorder/ScreenCast 节点（`grep -i 'record\|screencast'` 只命中 `/org/kde/KWin/EIS/InputCapture`） | **不可行（本版本没有该接口）** |
| portal `org.freedesktop.portal.ScreenCast` | 接口存在（`CreateSession`/`SelectSources`/`Start`）；KDE 侧实现也在（`org.freedesktop.impl.portal.desktop.kde`） | 可行，但**未验证**：起会话会弹用户授权对话框，本 Phase 未打扰 |
| portal `org.freedesktop.portal.Screenshot` | 接口存在；KDE 实现里含 `ScreenshotDialog`（标题 "Request Screenshot"）与 `screenshot.valid` 权限表项 → 会走用户授权 | 可作为授权失败时的回退 |
| `ffmpeg -f x11grab` | `ffmpeg -devices` 列出 `x11grab`；`DISPLAY=:1 ffmpeg … -i :1+0,0 -frames:v 1` 产出 320x240 PNG（797B，内容近乎单色） | 可用，但**只证明能跑**：XWayland 根窗口不呈现 Wayland 窗口内容，不能当作 X11 会话能力 |

---

## ⑧ 窗口截图"所见即所得" —— **已验证路线**（`probes/wysiwyg-verify.sh`）

测试窗口（自带 KWin 标题栏，`frame=600x428 @ (660,326)`、`client=600x400 @ (660,354)`）：

| 调用 | 返回尺寸 | 含义 |
| --- | --- | --- |
| `CaptureWindow(handle)`（无选项） | **600x400** | 只有**客户区**（无标题栏、无阴影）→ 不满足"所见即所得" |
| `CaptureWindow(handle, {include-decoration:true})` | **730x558** | 窗口 + 装饰 + **阴影边距** |
| `CaptureActiveWindow` | 600x400 | 同无选项行为（客户区） |
| `CaptureArea(frame 矩形)` | 600x428 | 屏幕上该矩形 |
| 其它选项（`include-shadow` / `include-cursor` / `native-resolution`） | 600x400 | 实测无效（不改变结果） |
| `include-decoration=true` + `include-shadow=true` | 730x558 | 与只用 `include-decoration` 相同 |

`include-decoration` 图像的几何（用测试窗口正中品红块定位，可信度高于模板匹配）：

```
decor 图 730x558；品红块 bbox=(360,276,10,10)
client 偏移=(65,81)  frame 偏移=(65,53)
边距：left=65 top=53 right=65 bottom=77     # Breeze 阴影上下不对称
屏幕矩形：IX=595 IY=273 IW=730 IH=558
```

内容一致性（这是"所见即所得"的关键判据）：

```
frame 区域（600x428+65+53）窗口图 vs 屏幕图：
  RMSE = 0.0159        AE(fuzz 2%) = 0.00086   → 标题栏+客户区与屏幕像素一致
客户区（600x400）窗口图 vs 从屏幕图裁出的客户区：
  RMSE = 0.0188                                 → 一致
阴影边距区（左 65px 条）窗口图 vs 屏幕图：
  RMSE = 0.95；窗口图 alpha 均值 ≈ 3/255（近乎透明），屏幕图 alpha = 255（壁纸不透明）
```

结论：`include-decoration=true` 给出的就是**窗口自身渲染出来的样子**（标题栏/圆角/边框在内，
外圈是带 alpha 的极淡阴影，没有把桌面背景合成进去）；它与屏幕像素的差异**只**出现在阴影边距。

- `alpha` 通道可用来定位 frame（不透明区包围盒实测 `602x430+64+52`，与品红块推出的
  `600x428+65+53` 差约 1px）→ 阴影边距**不必硬编码**，可运行时推导（±1px）。
- 另有一例（QQ 窗口）`CaptureActiveWindow` 返回 1043x806 而其 `frameGeometry` 是 957x720
  → **不能假设返回尺寸 == frame/client 尺寸**，必须按实际返回处理。

---

## 附：产物索引（均可复核）

| 文件 | 内容 |
| --- | --- |
| `probes/kwin-probe.js` | 窗口列表/几何/excludeFromCapture 的 KWin 脚本探测 |
| `probes/shot2probe.cpp` | ScreenShot2 调用（异步+读满即停） |
| `probes/testwindow.cpp` | 受控测试窗口（2x2 色块 + 品红定位块） |
| `probes/overlayprobe.cpp` | layer-shell 全屏覆盖层 + SIGUSR1 隐藏 |
| `probes/*.sh` | 授权/窗口列表、WYSIWYG、覆盖层自排除三组可重复实验 |
| `probes/out/` | 全部原始输出、PNG 证据、日志 |
| `probes/cmake/CMakeLists.txt` | 已验证的 CMake + Qt6 + LayerShellQt 组合写法 |

## 判定汇总

| 探测项 | 判定 |
| --- | --- |
| ① ScreenShot2 截图接口 + 授权规则 + 调用约束 | **已验证** |
| ② KWin 脚本窗口枚举 / frameGeometry / internalId | **已验证** |
| ③ layer-shell 覆盖层 + 自排除（`excludeFromCapture`） | **已验证** |
| ④ 输出与缩放（scale=1 单输出） | **已验证**；scale≠1 / 多输出 **未验证** |
| ⑤ KGlobalAccel 开发资源与注册前提 | **已验证**（默认键冲突待你裁决） |
| ⑥ 剪贴板持久性 | **已验证** |
| ⑦ 录制路径 | KWin 录制接口 **不可行**；portal ScreenCast **未验证**（需授权交互）；x11grab **可用但仅 XWayland 冒烟** |
| ⑧ 所见即所得窗口截图 | **已验证**（`include-decoration=true` + frame 定位） |
