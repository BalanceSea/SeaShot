<p align="center"><img src="resources/icons/seashot.png" alt="SeaShot" width="96"></p>
<h1 align="center">🌊 SeaShot</h1>

<p align="center"><strong>KDE Plasma 6 的截图 / 就地标注 / 贴图 / 长截图 / OCR / 录屏工具</strong><br>
在屏幕上就地标注，把灵感留下来 ✨　🐧 Linux · 🖥️ KDE Plasma 6 · ⚡ C++20 · 🧩 Qt 6 · 📸 Wayland / X11</p>

<p align="center"><a href="docs/images/capture-annotate.png"><img src="docs/images/capture-annotate.png" alt="就地标注：冻结选区 + 工具条" width="720"></a></p>

## ✨ 能做什么

| | | | |
| --- | --- | --- | --- |
| 📸 截图 | 悬停选窗口 / 拖框选区 | ✏️ 就地标注 | 9 种工具就地画，不弹新窗口 |
| 🫧 模糊 / 马赛克 | 在选区里遮盖敏感内容 | 📌 贴图 | 钉在屏幕上，可拖动、缩放、多张并存 |
| 📜 长截图 | 自己滚动，按「完成」结束 | 🔎 OCR | 识别选区文字，可复制全部 / 逐行 |
| 🎬 录制 | 选区录 GIF / MP4，录完选格式 | ⌨️ 全局快捷键 | 走系统门户授权，由你决定 |

## 🚀 安装

```bash
./install.sh --autostart    # 装依赖 → 构建 → 安装 → 登录自启（快捷键需常驻才响应）
./install.sh --skip-deps    # 依赖已就绪时：只构建 + 安装
```

Wayland 下请用安装脚本安装（它会写 KWin 截图授权声明并刷新 KDE 应用缓存）。
依赖表、各发行版差异、手动编译与 PATH 处理见 [构建与安装](docs/build.md)。

## 📸 用法

1. 按快捷键（或 `seashot --capture`）→ 单击窗口 / 拖框选区
2. 就地标注：工具条选工具，拖边框或 8 个手柄可继续改选区；`1`–`9` 切工具
3. `Enter` 或 ✓ 完成 —— **不弹结果窗口，直接复制到剪贴板**（按设置也可能自动保存）

另存 / 进编辑器 / 导出录制：托盘菜单「保存 / 导出上一次结果…」；全部快捷键、CLI 开关、配置键与输出标记见 [命令行与配置](docs/cli-and-config.md)。

## ✅ 验收

每一步交付都有可重复执行的验收脚本（截图主链路、编辑器、就地标注 A1–A3.1、选区调整 B、贴图 C、长截图 D、OCR E、全局快捷键 F），证据（截图 / 日志）与逐阶段记录只落在**本地** `docs/acceptance/`（已 gitignore，不进仓库）。

```bash
cd build && ctest --output-on-failure && cd ..            # 11 套单元测试
for s in phase1 phase2 phase3 phase4 phaseA1 phaseA2 phaseA3 phaseA3-color \
         phaseB phaseC phaseD phaseE phaseF; do
    bash scripts/acceptance-$s.sh || echo "FAILED: $s"; done   # 13 个验收套件
```

最近一次实测：`ctest` 11/11；`phase1` 9/0、`phase2` 4/0、`phase3` 7/0、`phase4` 7/0、`phaseA1` 6/0、`phaseA2` 10/0、`phaseA3` 13/0、`phaseA3-color` 7/0、`phaseB` 11/0、`phaseC` 10/0、`phaseE` 7/0、`phaseF` 8/0、`phaseD` 8/0（通过/失败；桌面被占用时 `phaseD` 会出 2 条 SKIP 而非 FAIL）。

## 🗂️ 文档

| 文档 | 内容 |
| --- | --- |
| [命令行与配置](docs/cli-and-config.md) | 全部 CLI 开关、12 个配置键、选区快捷键、输出标记 |
| [录制与导出](docs/recording.md) | PipeWire / 轮询回退、帧率与丢帧、中间文件、导出 GIF/MP4 |
| [构建与安装](docs/build.md) | 依赖表、各发行版、安装脚本开关、Wayland 授权 |
| [兼容性与限制](docs/limitations.md) | 后端差异、缩放、多屏、长截图与快捷键边界 |
| [交互与视觉规格](docs/ui-spec.md) | 界面规格与图标约定 |
| [演示图生成](docs/images/README.md) | 用真实控件重新生成 README 配图 |

> 本地开发文档（`docs/known-issues.md`、`docs/plan-inplace-editor.md`、`docs/next-session-plan.md`、`docs/acceptance/`）不入库，用 `ls docs/` 查看。
