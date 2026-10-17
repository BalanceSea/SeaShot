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
| `acceptance-phaseA1 / A2 / A3 / A3-color / B / C / D / E` | 6 / 10 / 13 / 7 / 11 / 10 / 6+2 跳过 / 7（D 的那 2 条是测试窗口被别的窗口盖住时的 SKIP，不是失败；桌面空闲时 8/0） |
| `acceptance-phase1 / 2 / 3 / 4` | 9 / 4 / 7 / 7 全过 |

## 顺带修掉的两条脆弱判据

- `phaseB` 的「保存的结果里确实有标注」原来拿**两次不同运行**的产物比 RMSE —— 桌面内容一变就假红
  （实测未标注 0.146 / 预览 0.095，看着像图里没标注，其实背景变了）。改成同一次运行内比
  （峰值差 ≥100 + 差异像素 ≥500，与背景无关），与 A3 的做法一致。
- `phaseD` 原来在「测试窗口被别的窗口盖住、只滚了一部分」时直接判 FAIL（实测 2260/2500）；
  现在会先**激活测试窗口**，仍被打断则报 **SKIP（环境遮挡）** —— 与 phase1 C 段一致。

## 首次运行自动发起（后来加的）

用户要求"应用自己注册、用户决定授权"，于是常驻实例**第一次**启动时会自动发起一次：
- 条件（全满足才发起）：`shortcut/portalAsked` 未置位、`shortcut/hotkey` 为空、门户可用；
- **先静默 `ListShortcuts`**：如果门户里已经有绑定（之前授权过）→ 什么都不做，只打
  `HOTKEY_FIRST_RUN already-authorized trigger=...（不再打扰）`；
  真的没有绑定才 `BindShortcuts` → 于是系统弹授权对话框，由用户允许/拒绝；
- 无论结果如何都会把 `shortcut/portalAsked` 置 true —— **只问一次**（之后想再问就点设置里的按钮）。
- 验收/自动化用 `SEASHOT_NO_FIRST_RUN_ASK=1` 关掉（否则系统对话框会挡住验收；phase3/phaseF 已加）。

实现时踩到一个顺序坑：一开始"是否已授权"和"要不要发起"各自发了一次 `ListShortcuts`，
先到的那个空应答被当成"没有绑定"→ 多发起了一次 bind —— 而 **bind 会带着 preferred_trigger 写回绑定**，
有覆盖用户已有按键的风险（正是 ISSUE-014 第 5 条）。现在改成：只发**一次** list，
同一个结果同时用于两件事；并且"已授权就绝不 bind"。

## 同一时间只让**一条**机制生效（用户反馈"为什么会注册两个截图快捷键"）

一开始两条路各自为政：门户那条注册了 `screenshot=Ctrl+Alt+A`，KGlobalAccel 那条也按配置注册了
`capture=Ctrl+Alt+A` —— 系统设置里就出现**两条 SeaShot 快捷键绑在同一个键**。现在的规则：

- **门户已授权 → KGlobalAccel 不注册**（并且启动时把 KGlobalAccel 那条**解绑**成 `none`），
  托盘提示文字也标成「Ctrl+Alt+A（门户）」。系统设置里只剩一条有键的。
- **门户没授权/不可用 → 走 KGlobalAccel**（用配置里的键）。
- 用户显式 `--set-hotkey`（或界面里录键）→ 那是明确的"我要用 KGlobalAccel"，此时门户让位。
- 启动瞬间也不会出现"两条都绑着"的抖动：**先等门户状态确定**（`m_portalStateKnown`）再决定注册谁。

还有两条"查询/验收别改全局状态"的修正：
- **`--hotkey-status` / `--print-config` 原来会顺手 `applyHotkey()`** —— 查一次状态就把
  KGlobalAccel 那条又绑回去（于是系统设置里又变两条，实测踩过）。现在查询是**纯读**：
  后端的 `ensureHotkeyAction()` 只建 QAction、不碰绑定。
- phase3 的 D 段测完恢复原值时，若**门户那条在生效**就恢复成 `none`（不再重复绑 KGlobalAccel）。

另外修了一个真 bug：**空序列原来是"什么都不做"**，所以 `--set-hotkey none` 其实解不掉旧键
（KGlobalAccel 仍按配置里的旧值生效）。现在空序列 = **明确解绑**。

实测：
```
[info] [app] 等门户状态确定后再决定是否注册 KGlobalAccel
HOTKEY_PORTAL_WINS 停止注册 KGlobalAccel（避免两条快捷键）
HOTKEY_PORTAL_STATE authorized id=screenshot trigger=Ctrl+Alt+A
→ ~/.config/kglobalshortcutsrc:  capture=none   screenshot=Ctrl+Alt+A
```

## 退出 SeaShot 后快捷键为什么按不动

不是 bug：快捷键按下后必须由**正在运行的 SeaShot** 接管（KGlobalAccel 与门户都是如此 ——
它们只是把"按键事件"交给注册它的进程；进程不在就没人处理）。
想让 `Ctrl+Alt+A` 一直可用，就让常驻实例一直在：托盘常驻即可，或者启用开机自启：
`./install.sh --autostart`（写 `~/.config/autostart/seashot.desktop`）。

## 关于 kglobalshortcutsrc 里的测试残留条目

开发/调试过程中留下过 `screenshot=(none)`、`seashot-test-new=(none)` 两个空条目。
**清理不掉**：把 `~/.config/kglobalshortcutsrc` 里对应的行删掉、并重启 `kglobalacceld`
与 `xdg-desktop-portal-kde` 之后，只要再查一次（portal 会把它"记得"的 id 集合重新注册回去），
它们就会再出现。它们没有绑键（`none`）、界面里不显示、不影响任何功能 —— 只能这样留着。

## 还没做的

- **不主动弹窗**：目前首次授权要用户点一下设置/托盘里的入口（或命令行）。要不要在"第一次运行"
  就自动发起一次（从而立刻弹授权对话框）？这会在启动时打扰用户，所以我没做 —— 你说要就加。
- **不同 id 的历史遗留**：本机因为调试留下了 `screenshot` / `seashot-test-new` 两个空条目
  （`kglobalshortcutsrc` 里 `=none`），它们不影响使用（没有键），但看着多余。
