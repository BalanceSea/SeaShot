#!/usr/bin/env bash
# Phase E 验收脚本：OCR 文字识别
#
# 用法： bash scripts/acceptance-phaseE.sh
# 证据落在 docs/acceptance/evidence/
#
# 判据（全部自动判定）：
#   1) 依赖就绪：tesseract 可用，且语言包里同时有 chi_sim 与 eng
#   2) 中文识别：对一张**已知文本**的合成图识别，文本对得上（允许个别字错）+ 报出耗时
#   3) 英文识别：同上，关键短语必须完全命中
#   4) 交互入口 Ctrl+O：识别 → 弹出结果面板 → 两个复制入口都能把内容送进剪贴板
#   5) 单测（tst_ocrengine，本机装了 tesseract 就真跑识别）
#   6) 缺语言包/空图能给出明确错误，而不是静默返回空文本
#
# 注意：会短暂弹出一个 OCR 结果面板窗口（交互入口那一段）。
set -u
cd "$(dirname "$0")/.."

SEASHOT="$HOME/.local/bin/seashot"
OUT="docs/acceptance/evidence"
ZH_IMG="$OUT/e-ocr-zh.png"
EN_IMG="$OUT/e-ocr-en.png"
mkdir -p "$OUT" docs/acceptance
PASS=0
FAIL=0

report() { # $1 名称 $2 0/1 $3 细节
    if [ "$2" = 1 ]; then
        printf 'PASS  %-40s %s\n' "$1" "$3"
        PASS=$((PASS + 1))
    else
        printf 'FAIL  %-40s %s\n' "$1" "$3"
        FAIL=$((FAIL + 1))
    fi
}

# 从 OCR 输出里取出识别文本（BEGIN/END 之间）
ocr_text() { sed -n '/^OCR_TEXT_BEGIN$/,/^OCR_TEXT_END$/p' "$1" | sed '1d;$d'; }

echo "== Phase E 验收（OCR 文字识别）=="
echo "二进制: $SEASHOT ($(stat -c '%y' "$SEASHOT" | cut -d. -f1))"
echo

### A) 依赖 ---------------------------------------------------------------------
LANGS=$(tesseract --list-langs 2>/dev/null | tail -n +2 | tr '\n' ' ')
HAS_ZH=$(grep -cw chi_sim <<<"$LANGS" || true)
HAS_EN=$(grep -cw eng <<<"$LANGS" || true)
if [ "$HAS_EN" = 1 ] && [ "$HAS_ZH" = 1 ]; then
    report "E. 依赖：tesseract + 中英语言包" 1 "语言包：$(tr ' ' ',' <<<"$LANGS" | sed 's/,$//')"
else
    report "E. 依赖：tesseract + 中英语言包" 0 \
        "缺语言包（chi_sim=$HAS_ZH eng=$HAS_EN）；装：sudo pacman -S tesseract tesseract-data-chi_sim tesseract-data-eng"
fi

### B/C) 合成已知文本的图 → 识别 → 比对 ------------------------------------------
CJK_FONT=$(fc-match -f '%{file}' "Noto Sans CJK SC:style=Regular")
[ -n "$CJK_FONT" ] || CJK_FONT=$(fc-match -f '%{file}' "PingFang SC")
LATIN_FONT=$(fc-match -f '%{file}' sans-serif)
magick -size 1400x220 xc:white -font "$CJK_FONT" -pointsize 72 -fill black \
    -annotate +40+140 '你好世界这是识别测试' "$ZH_IMG" 2>/dev/null
magick -size 1400x220 xc:white -font "$LATIN_FONT" -pointsize 72 -fill black \
    -annotate +40+140 'Hello World OCR test 12345' "$EN_IMG" 2>/dev/null

# 中文
timeout 60 "$SEASHOT" --ocr "$ZH_IMG" > "$OUT/e-ocr-zh.txt" 2>&1
ZH_CODE=$?
ZH_TEXT=$(ocr_text "$OUT/e-ocr-zh.txt" | tr -d '\n')
ZH_MS=$(sed -nE 's/^OCR ok .*elapsedMs=([0-9]+).*/\1/p' "$OUT/e-ocr-zh.txt" | head -1)
ZH_SCORE=$(python3 - "$ZH_TEXT" <<'PY'
import sys, unicodedata
want = "你好世界这是识别测试"
got = "".join(c for c in sys.argv[1] if not c.isspace() and c not in "，。！？、,.!?;:")
hits = sum(1 for c in want if c in got)
print(f"{hits}/{len(want)}")
PY
)
ZH_OK=$(python3 -c "
got=sum(int(x) for x in '${ZH_SCORE%%/*}'.split())
print(1 if got >= 7 else 0)")
if [ "$ZH_CODE" = 0 ] && [ "$ZH_OK" = 1 ]; then
    report "E. 中文识别（与已知文本比对）" 1 \
        "识别=\"$ZH_TEXT\"（命中 ${ZH_SCORE}），耗时 ${ZH_MS}ms"
else
    report "E. 中文识别（与已知文本比对）" 0 \
        "识别=\"$ZH_TEXT\"（命中 ${ZH_SCORE}）退出码=$ZH_CODE；见 $OUT/e-ocr-zh.txt"
fi

# 英文
timeout 60 "$SEASHOT" --ocr "$EN_IMG" --ocr-langs eng > "$OUT/e-ocr-en.txt" 2>&1
EN_CODE=$?
EN_TEXT=$(ocr_text "$OUT/e-ocr-en.txt" | tr -d '\n')
EN_MS=$(sed -nE 's/^OCR ok .*elapsedMs=([0-9]+).*/\1/p' "$OUT/e-ocr-en.txt" | head -1)
if [ "$EN_CODE" = 0 ] && [[ "$EN_TEXT" == *"Hello World"* ]] && [[ "$EN_TEXT" == *"12345"* ]]; then
    report "E. 英文识别（关键短语必须命中）" 1 \
        "识别=\"$EN_TEXT\"，耗时 ${EN_MS}ms（--ocr-langs eng 生效）"
else
    report "E. 英文识别（关键短语必须命中）" 0 \
        "识别=\"$EN_TEXT\" 退出码=$EN_CODE；见 $OUT/e-ocr-en.txt"
fi

### D) 交互入口：Ctrl+O → 结果面板 → 两个复制入口 ---------------------------------
INPLACE_LOG="$OUT/e-ocr-inplace.txt"
wl-copy --clear 2>/dev/null
SEASHOT_INPLACE_DEMO_ACTION=ocr timeout 120 "$SEASHOT" --inplace-demo "$OUT/e-ocr-inplace" \
    --no-copy > "$INPLACE_LOG" 2>&1
INPLACE_CODE=$?
OCR_OK=$(grep -m1 '^OCR ok ' "$INPLACE_LOG")
WINDOW_OK=$(grep -m1 '^OCR_WINDOW ok=1' "$INPLACE_LOG")
COPY_ALL=$(grep -m1 '^OCR_COPY ok chars=' "$INPLACE_LOG")
COPY_LINE=$(grep -m1 '^OCR_COPY_LINE ok chars=' "$INPLACE_LOG")
CLIP=$(wl-paste 2>/dev/null | head -1)
IN_OCR_TEXT=$(python3 - "$CLIP" "$INPLACE_LOG" <<'PY'
import re, sys
clip = sys.argv[1].strip()
text = open(sys.argv[2], encoding='utf-8').read()
m = re.search(r'OCR_TEXT_BEGIN\n(.*?)\nOCR_TEXT_END', text, re.S)
body = m.group(1) if m else ""
print(1 if clip and clip in body else 0)
PY
)
if [ -n "$OCR_OK" ] && [ -n "$WINDOW_OK" ] && [ -n "$COPY_ALL" ] && [ -n "$COPY_LINE" ] \
    && [ "$IN_OCR_TEXT" = 1 ] && grep -q '^INPLACE_DEMO ALL PASS' "$INPLACE_LOG"; then
    report "E. 交互入口 Ctrl+O → 面板 → 复制" 1 \
        "$OCR_OK；$WINDOW_OK；$COPY_ALL；$COPY_LINE；剪贴板里确实是识别结果里的那一行（\"${CLIP:0:24}…\"）"
else
    report "E. 交互入口 Ctrl+O → 面板 → 复制" 0 \
        "ocr='${OCR_OK:-无}' window='${WINDOW_OK:-无}' copyAll='${COPY_ALL:-无}' copyLine='${COPY_LINE:-无}' 剪贴板属于识别结果=$IN_OCR_TEXT（退出码 $INPLACE_CODE）"
fi
wl-copy --clear 2>/dev/null

### E) 错误路径：空图必须报错 ----------------------------------------------------
printf '' > "$OUT/e-ocr-empty.txt"
timeout 30 "$SEASHOT" --ocr "$OUT/e-ocr-empty.txt" > "$OUT/e-ocr-empty.out" 2>&1
EMPTY_CODE=$?
if grep -q '^OCR_ERROR' "$OUT/e-ocr-empty.out" && [ "$EMPTY_CODE" != 0 ]; then
    report "E. 错误路径：读不到的图会明确报错" 1 \
        "$(grep -m1 '^OCR_ERROR' "$OUT/e-ocr-empty.out")（退出码 $EMPTY_CODE，不是静默返回空文本）"
else
    report "E. 错误路径：读不到的图会明确报错" 0 "见 $OUT/e-ocr-empty.out"
fi

### F) 单测 ---------------------------------------------------------------------
if ctest --test-dir build -R ocr > "$OUT/e-unit.txt" 2>&1; then
    report "E. 单测（本机装了 tesseract 就真跑识别）" 1 \
        "$(grep 'tests passed' "$OUT/e-unit.txt" | tail -1)"
else
    report "E. 单测（本机装了 tesseract 就真跑识别）" 0 "见 $OUT/e-unit.txt"
fi

if [ -f "$ZH_IMG" ] && [ -f "$EN_IMG" ]; then
    report "E. 视觉证据落盘" 1 \
        "e-ocr-zh.png / e-ocr-en.png（已知文本的合成图）+ e-ocr-*.txt（识别结果）"
else
    report "E. 视觉证据落盘" 0 "缺少合成测试图"
fi

echo
echo "== 汇总: $PASS 通过 / $FAIL 失败 =="
echo "证据: $OUT/e-ocr*.txt / e-ocr-*.png"
[ "$FAIL" = 0 ]
