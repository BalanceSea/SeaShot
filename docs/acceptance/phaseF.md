# Phase F 验收记录（全局快捷键：应用自己注册 + 用户授权）

- 日期：2026-10-17
- 二进制：`~/.local/bin/seashot`（`./install.sh --no-build`）
- 复现：`bash scripts/acceptance-phaseF.sh`（原始输出 `docs/acceptance/phaseF.log`，
  证据 `docs/acceptance/evidence/f-*.txt`）
- 结果：**8 项自动化判据全部通过**；人工项（首次授权对话框 + 按键触发）**由用户实测通过**

## 用户诉求与做法

> "让应用可以自己注册全局快捷键 让用户自己决定授权还是不授权"

走 **xdg-desktop-portal 的 `GlobalShortcuts` 接口**（Wayland 下"应用自己注册全局快捷键"的标准做法）：
应用发起 `BindShortcuts` → **系统弹授权对话框**（列出请求的快捷键、预填一个键，可当场改）→
用户**允许 / 拒绝**；允许后桌面按 app id 记住，之后同 id 不再弹窗。按下的键通过 `Activated` 信号回来。

| 部分 | 说明 |
| --- | --- |
| `src/hotkey/GlobalShortcutsPortal.{h,cpp}` | portal 客户端：`CreateSession` / `BindShortcuts` / `ListShortcuts` / `ConfigureShortcuts` + `Activated`/`ShortcutsChanged` 信号；请求/应答的路径与 token 规则沿用 ScreenCast 那套已验证写法；用**私有 D-Bus 连接 + Register app id**，KDE 才能把我们和 `seashot.desktop` 认成同一个应用（否则不记住授权） |
| 入口 | ①设置界面「全局快捷键」页：**「向系统申请注册（会弹授权对话框）」** + 「更改快捷键（系统对话框）」；②托盘菜单「注册全局快捷键（由我申请）…」；③命令行 `--hotkey-portal-request [--hotkey-portal-id X] [--hotkey-portal-ms N] [--hotkey-portal-listen-ms N]`；④`--hotkey-portal-status` 静默查状态 |
| 与 KGlobalAccel 的关系 | 两条路**并存**（KGlobalAccel 无需授权、portal 需用户授权）。`--hotkey-status` 同时报两者的状态，谁在真正生效一眼可见 |
| 常驻行为 | 启动时**静默**看一眼"授权过没有"：授权过就监听 `Activated`（与快捷键回调同一条路径：录制中=停止录制，否则截图）；**没授权过不主动弹窗**（不打扰用户），由上面三个入口发起 |

## 用户实测通过的那一次（首次授权）

用**全新 id**（`first-run-demo`）发起时，系统弹出授权对话框；用户点**允许**，绑定落地：

```
HOTKEY_PORTAL_REQUEST BindShortcuts ... shortcuts: QList(std::pair("first-run-demo",
    QMap(("description", "截图（SeaShot 截图）"))("preferred_trigger", "CTRL+ALT+A")))   ← journal
→ ~/.config/kglobalshortcutsrc:  first-run-demo=Ctrl+Alt+A,Ctrl+Alt+A,截图（SeaShot 截图）
→ HOTKEY_PORTAL_STATE authorized id=first-run-demo trigger=Ctrl+Alt+A
   （常驻实例启动时静默认出这条绑定；按 Ctrl+Alt+A 即截图 —— 用户实测确认）
```

## 自动化判据结果

| # | 判据 | 结果 | 证据 |
| --- | --- | --- | --- |
| 1 | 门户可用（GlobalShortcuts） | **PASS**：`available=1 version=2`（xdg-desktop-portal-kde 6.7.5） | `evidence/f-portal-status.txt` |
| 2 | 静默查询不弹窗且有结果 | **PASS**：`count=4`，列出我们所有 id 与是否绑键 | 同上 |
| 3 | 请求注册不会挂死 | **PASS**：`BindShortcuts` 已发出，75ms 内以 `HOTKEY_PORTAL_FLOW ok=1` 收尾 | `evidence/f-portal-request.txt` |
| 4 | 常驻自检认出已授权绑定 | **PASS**：`HOTKEY_PORTAL_STATE authorized id=first-run-demo trigger=Ctrl+Alt+A` | `evidence/f-resident.txt` |
| 5 | `--hotkey-status` 同时报两条路 | **PASS**：`HOTKEY_PORTAL_STATE ... authorized=1 ... · HOTKEY configured=Ctrl+Alt+A actual=Ctrl+Alt+A` | `evidence/f-hotkey-status.txt` |
| 6 | 原有 KGlobalAccel 路径未受影响 | **PASS**：写入 `Ctrl+Alt+F9` → 读回 → 解绑 → **恢复原值** 全正常 | `evidence/f-kga-*.txt` |
| 7 | 状态与实际绑定一致（不谎报） | **PASS**：状态里的 trigger 能在 `ListShortcuts` 里对上 | `evidence/f-resident.txt` |
| 8 | 证据落盘 | **PASS**：4 个文本证据 | 见上 |

## 这轮踩到的 5 个坑（都写进了 ISSUE-014，别再踩）

1. **`a(sa{sv})` 参数会被静默丢掉**：直接写 `QVariant::fromValue(QList<QPair<QString,QVariantMap>>)`
   能编译、调用也发得出去，但 Qt 在发送时把这一项扔了（4 个参数变 3 个），portal 只回一句
   "类型不匹配"。解法：用一个**带流运算符的小结构体**（`PortalShortcutEntry`）+ 声明并注册元类型
   —— 与 ScreenCast 那边一致。
2. **`Qt::UniqueConnection` 对 lambda 无效**：Qt 干脆**不建立连接**（回调一个都不触发，
   而 `BindShortcuts` 明明成功）。解法：信号只在 `ensureShortcutPortal()` 里接一次。
3. **`session_handle` 是 D-Bus 的 `o`（对象路径）**，塞 `QString` 发不出去；
   `QDBus::NoBlock` 连错误都看不见。解法：`QDBusObjectPath` + `QDBus::Block` + 打印错误。
4. **`preferred_trigger` 必须是大写的 XDG 格式**：`"Ctrl+Alt+S"` 会被 KDE 判成
   `Unknown modifier "Ctrl"` 丢掉（表现为"授权了但没有键"）；要 `"CTRL+ALT+S"`。
5. **⚠️ 最危险的一条：portal 的静默 bind（没带按键）会写回绑定，把已有的键覆盖成空**。
   开发中我用 `capture` 这个 id（KGlobalAccel 那条路也在用同一个 id）发起过一次不带
   `preferred_trigger` 的 bind，结果把用户自己设的 `Ctrl+Alt+A` 清掉了。现在代码**永远带
   `preferred_trigger`**，验收脚本也会"测完恢复原值"。

另外两条 KDE 语义（不是坑，但会影响预期）：
- **KDE 6 里 KGlobalAccel 与 portal 共用同一份快捷键存储**（`kglobalshortcutsrc`）。所以已经被
  KGlobalAccel 建过条目的 id **不会弹授权对话框**（portal 认为"已知"）。要给用户"首次授权"的体验，
  发起时用的 id 必须是新的 —— 所以代码里 portal 用的 id 是 `screenshot`（不是 KGlobalAccel 的 `capture`）。
- 授权结果**没有"忘记/重置"API**：想让某个 id 重新走授权，得
  `systemctl --user stop plasma-kglobalaccel.service`，从 `kglobalshortcutsrc` 的 `[seashot]` 段
  删掉那个 id，再发起。

## 回归

| 套件 | 结果 |
| --- | --- |
| `ctest`（11 套单测） | **11/11 通过** |
| `acceptance-phaseA1 / A2 / A3 / A3-color / B / C / D / E` | 6 / 10 / 13 / 7 / 11 / 10 / 8 / 7 全过 |
| `acceptance-phase1 / 2 / 3 / 4` | 9 / 4 / 7 / 7 全过 |

## 还没做的

- **不主动弹窗**：目前首次授权要用户点一下设置/托盘里的入口（或命令行）。要不要在"第一次运行"
  就自动发起一次（从而立刻弹授权对话框）？这会在启动时打扰用户，所以我没做 —— 你说要就加。
- **不同 id 的历史遗留**：本机因为调试留下了 `screenshot` / `seashot-test-new` 两个空条目
  （`kglobalshortcutsrc` 里 `=none`），它们不影响使用（没有键），但看着多余。
