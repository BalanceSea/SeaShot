# Phase 3 验收记录（配置界面 + 托盘菜单 + 快捷键）

- 日期：2026-10-16
- 二进制：`~/.local/bin/seashot`
- 复现：`bash scripts/acceptance-phase3.sh`（原始输出 `docs/acceptance/phase3.log`）
- 结果：**7 项自动化判据全部通过**

## 交付内容（对照 PLAN 第 4 节 Phase 3）

| 需求 | 状态 | 位置 |
| --- | --- | --- |
| 配置界面（PLAN 第 2.6 节全部键） | 完成：截图后三项、保存目录/模板/阴影边距、浮层超时、常驻托盘、快捷键、GIF 帧率 | `src/app/SettingsDialog.{h,cpp}` |
| 改动即时生效 | 完成：每个控件变化即写回 `QSettings` 并发 `Settings::changed` → `Application::applySettings()` | 同上 + `src/app/Application.cpp` |
| 持久化 | 完成：`~/.config/seashot/seashot.conf`；「恢复默认」= 清空配置 | `src/app/Settings.cpp` |
| 托盘菜单 | 完成：截图 / 设置… / KDE 快捷键设置… / 退出（`KStatusNotifierItem`） | `src/app/Tray.cpp` |
| 快捷键绑定入口 | 完成：界面里 `QKeySequenceEdit` 直接写进 KGlobalAccel 并落盘；另有按钮跳 KDE 快捷键 KCM | `src/app/SettingsDialog.cpp` |
| 新 CLI | `--settings`、`--set-hotkey`、`--hotkey-status`、`--print-config` | `src/main.cpp`、`src/app/Application.cpp` |

## 逐条验收结果

| # | 判据 | 结果 | 证据 |
| --- | --- | --- | --- |
| A | 单测（配置写回 / 托盘菜单 / 配置持久化） | **PASS**：7 个测试套件全过（`tst_settingsdialog` 5 例、`tst_tray` 2 例） | `evidence/p3-unit.txt` |
| B | `--print-config` 键齐全 | **PASS**：13 项齐备（check/目录/模板/快捷键/帧率/浮层超时/托盘/后端） | `evidence/p3-print-config.txt` |
| C | 配置改后对新进程立即生效 | **PASS**：写 `openEditorAfterCapture=true` → 新进程 `--rect … --no-copy` 输出 `EDITOR_OPEN` 且**不弹浮层** | `evidence/p3-open-editor.txt` |
| D | 快捷键写入 / 读回 / 换键 / 解绑 | **PASS**：`Ctrl+Alt+S` → 读回 `Ctrl+Alt+S`；换 `Ctrl+Shift+F9` → 读回一致；解绑 → `(未绑定)`（KGlobalAccel 跨进程持久化） | `evidence/p3-hotkey-status.txt` |
| E | 托盘 SNI 注册 | **PASS**：watcher 新增 `:1.34672/StatusNotifierItem`，`Category=ApplicationStatus`、`Status=Active`、`Title=SeaShot` | `evidence/p3-tray.txt`、`p3-resident.txt` |
| E | 关掉「常驻托盘」后不注册 | **PASS**：`ui/trayPersistent=false` 时 watcher 无新增项 | `evidence/p3-resident-notray.txt` |
| F | 配置界面可启动 | **PASS**：`--settings` 输出 `SETTINGS_OPENED file=~/.config/seashot/seashot.conf` | `evidence/p3-settings.txt` |

界面实拍（BreezeLight 主题下的真实渲染）：`evidence/p3-settings-dialog.png`
——「截图后」「保存」「界面」「全局快捷键」「GIF 录制（Phase 4 生效）」五组，底部是配置文件路径与
「恢复默认 / 关闭」。

## 这轮修掉的一个真 bug

`Tray` 析构里手动 `delete m_menu` → 与 `KStatusNotifierItem` 自己接管 contextMenu 所有权冲突，
**double free**（单测直接崩到 `0x0`）。这个 bug 在生产路径上同样致命：配置界面里把「常驻托盘」
关掉时 `Application::applySettings()` 会销毁 Tray → 直接崩。现在不手动删，交给 item 管理。

（同类教训：写完 `tst_tray` 才暴露出来 —— 面板图标这种"看得见但没法自动点"的东西，
至少要把菜单项与注册状态做成可断言的对象。）

## 人工项

1. 面板上托盘图标的外观（菜单内容已由 `tst_tray` 断言、SNI 注册已由 E 段验证）。
2. 在配置界面里真手点一遍快捷键（`QKeySequenceEdit` 捕获按键 → 立刻生效）。
3. 「打开 KDE 快捷键设置…」是否真的拉起了「系统设置 → 快捷键」。

## 环境说明

托盘图标要求桌面提供 `StatusNotifierHost` + `StatusNotifierWatcher`。本机两者都有
（`IsStatusNotifierHostRegistered=true`），所以 E 段能真实验证；若换到没有 host 的桌面，
脚本会把这一段判为 SKIP 而不是 FAIL。
