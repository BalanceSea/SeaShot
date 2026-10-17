# Phase E 验收记录（OCR 文字识别）

- 日期：2026-10-17
- 二进制：`~/.local/bin/seashot`（`./install.sh --no-build`）
- 复现：`bash scripts/acceptance-phaseE.sh`（原始输出 `docs/acceptance/phaseE.log`，
  证据 `docs/acceptance/evidence/e-ocr*`）
- 结果：**7 项自动化判据全部通过**

## 交付内容（对应用户诉求 ⑧「OCR 文字识别」）

| 部分 | 说明 |
| --- | --- |
| `src/ocr/OcrEngine.{h,cpp}` | 包一层系统 `tesseract` 命令行：**图片走 stdin、文本走 stdout** —— 不落中间文件、不需要 tesseract 开发包。同步 + 异步两个入口（界面走异步，不卡事件循环）；语言包缺失**明确报错**，默认语言自动选 `chi_sim+eng` |
| `src/ocr/OcrWindow.{h,cpp}` | 结果面板（普通窗口，与编辑器一致）：文本只读可选中，按钮「复制全部」「复制选中」（没选中时退化为"复制光标所在行"），`Esc` 关闭；状态栏给语言/psm/行数/耗时 |
| `util/Clipboard::copyText` | 纯文本进剪贴板（`wl-copy` + `text/plain;charset=utf-8`，失败回退 QClipboard） |
| 入口 | ①就地标注工具条新增「OCR」格 + **`Ctrl+O`**（识别"屏幕上看到的那张选区图"，含标注）；②命令行 `seashot --ocr <png> [--ocr-langs chi_sim+eng]`，结果打到 stdout（带 `OCR_TEXT_BEGIN/END` 标记，便于脚本取） |

**外部依赖**（已确认就绪）：`tesseract` + `tesseract-data-chi_sim` + `tesseract-data-eng`；
`tesseract --list-langs` = `afr, chi_sim, eng, osd`。缺包时错误信息里直接给出安装命令。

## 自动化判据结果

| # | 判据 | 结果 | 证据 |
| --- | --- | --- | --- |
| 1 | 依赖：tesseract + 中英语言包 | **PASS**：语言包 `afr,chi_sim,eng,osd` | `evidence/e-ocr-*.txt` |
| 2 | 中文识别（与已知文本比对） | **PASS**：合成图 `你好世界这是识别测试` → 识别 `你 好 世 界 这 是 识 别 测 试`，**命中 10/10**，**390ms** | `evidence/e-ocr-zh.txt`、`e-ocr-zh.png` |
| 3 | 英文识别（关键短语必须命中） | **PASS**：`Hello World OCR test 12345` → 完全一致，**120ms**（`--ocr-langs eng` 生效） | `evidence/e-ocr-en.txt`、`e-ocr-en.png` |
| 4 | 交互入口 `Ctrl+O` → 面板 → 复制 | **PASS**：`OCR ok lines=12 535ms` → 面板弹出（`OCR_WINDOW ok=1`）→ 复制全部 139 字 + 复制当前行 15 字，且**剪贴板内容确实来自识别结果** | `evidence/e-ocr-inplace.txt` |
| 5 | 错误路径：读不到的图会明确报错 | **PASS**：`OCR_ERROR 无法读取 …` + 退出码 2（不是静默返回空文本） | `evidence/e-ocr-empty.out` |
| 6 | 单测（本机装了 tesseract 就真跑识别） | **PASS**：`tst_ocrengine`（语言包探测 / 英文关键短语 / 中文命中率 / 空图报错 / 显式语言）；没装 tesseract 时 `QSKIP` 而不是判失败 | `evidence/e-unit.txt` |
| 7 | 视觉证据落盘 | **PASS**：`e-ocr-zh.png` / `e-ocr-en.png`（已知文本的合成图）+ 识别结果文本 | 见上 |

中文识别实测（中文 OCR 会在字之间插空格，这是 tesseract 的正常行为）：

```
OCR ok lang=chi_sim+eng psm=3 lines=1 elapsedMs=390
你 好 世 界 这 是 识 别 测 试      ← 与合成图的文本逐字一致（10/10）
```

## 回归

| 套件 | 结果 |
| --- | --- |
| `ctest`（11 套单测） | **11/11 通过** |
| `acceptance-phaseA1 / A2 / A3 / A3-color / B / C / D` | 6 / 10 / 13 / 7 / 11 / 10 / 8 全过 |
| `acceptance-phase1 / 2 / 3 / 4` | 9 / 4 / 7 / 7 全过 |

顺带修掉两条**内容相关的脆弱判据**（这轮全套跑出来的）：
- `phaseA3` 的"结果里确实有标注"原来拿**两次不同运行**的产物比 RMSE，桌面内容一变就假红；
  改成同一次运行内比较（峰值差 ≥100 + 差异像素 ≥500，与背景无关）。
- `phase4` 的"8 秒录制"原来要求**文件体积**大于 3 秒那次 —— 而 GIF 体积只取决于画面内容
  （静画面能小到 1/3）。改成看帧数与耗时（8s 帧数 ≥ 3s 的两倍、耗时 ≥7s），体积只作参考。

## 已知限制

- 识别质量取决于**字体大小与清晰度**：实测 72px 的合成文字 100% 正确；
  屏幕截图里的小字（12–14px 终端文本）会错一些字（面板里可直接看出来，也能只复制对的那几行）。
- 语言包是外部依赖：缺 `chi_sim`/`eng` 时功能不可用（但会明确告知装什么）。
- 版面分析用的是 tesseract 默认 `--psm 3`（自动分页）；多栏/表格的阅读顺序可能不理想。
