#!/usr/bin/env bash
# Phase 2 验收脚本：编辑器（标注模型/撤销重做/渲染导出/不留半成品文件）
#
# 用法： bash scripts/acceptance-phase2.sh
# 证据落在 docs/acceptance/evidence/
#
# 说明：真实鼠标绘制/点按钮属人工项（见 docs/acceptance/phase2.md），
# 这里覆盖可自动化的部分：单测、渲染端到端、导出尺寸、不留半成品文件。
set -u
cd "$(dirname "$0")/.."

SEASHOT="$HOME/.local/bin/seashot"
OUT="docs/acceptance/evidence"
mkdir -p "$OUT" docs/acceptance
PASS=0
FAIL=0

report() { # $1 名称 $2 0/1 $3 细节
    if [ "$2" = 1 ]; then
        printf 'PASS  %-36s %s\n' "$1" "$3"
        PASS=$((PASS + 1))
    else
        printf 'FAIL  %-36s %s\n' "$1" "$3"
        FAIL=$((FAIL + 1))
    fi
}

echo "== Phase 2 验收 =="
echo "二进制: $SEASHOT ($(stat -c '%y' "$SEASHOT" | cut -d. -f1))"
echo

### A) 单元测试：撤销栈 / 几何命中 / 渲染 / 画布交互 ----------------------------
if ctest --test-dir build -R 'tst_annotation|tst_selection|tst_screenmapper' \
    > "$OUT/p2-unit.txt" 2>&1; then
    CASES=$(grep -cE '^PASS' "$OUT/p2-unit.txt" || true)
    report "A. 单测（撤销/命中/渲染/交互）" 1 "$(grep 'tests passed' "$OUT/p2-unit.txt" | tail -1)"
else
    report "A. 单测（撤销/命中/渲染/交互）" 0 "见 $OUT/p2-unit.txt"
fi

### B) 标注渲染端到端 + 导出尺寸不变 -----------------------------------------
"$SEASHOT" --rect 0,0,400,300 --save "$OUT/p2-source.png" --no-copy > "$OUT/p2-capture.txt" 2>&1
"$SEASHOT" --annotate-in "$OUT/p2-source.png" --annotate-out "$OUT/p2-annotated.png" \
    > "$OUT/p2-annotate.txt" 2>&1
if grep -q '^ANNOTATED' "$OUT/p2-annotate.txt"; then
    SRC_SIZE=$(magick identify -format '%wx%h' "$OUT/p2-source.png")
    OUT_SIZE=$(magick identify -format '%wx%h' "$OUT/p2-annotated.png")
    if [ "$SRC_SIZE" = "$OUT_SIZE" ]; then
        report "B. 导出尺寸 == 源图尺寸" 1 "两者都是 $SRC_SIZE（$(cat "$OUT/p2-annotate.txt")）"
    else
        report "B. 导出尺寸 == 源图尺寸" 0 "源 $SRC_SIZE vs 导出 $OUT_SIZE"
    fi
else
    report "B. 导出尺寸 == 源图尺寸" 0 "$(cat "$OUT/p2-annotate.txt")"
fi

### C) 五种工具都真的画上去了（按网格核对差异分布） ---------------------------
GRID=$(python3 - "$OUT/p2-source.png" "$OUT/p2-annotated.png" <<'PY'
import subprocess, sys

def raw(path):
    return subprocess.run(['magick', path, '-depth', '8', 'rgb:-'],
                          capture_output=True).stdout

src, out = raw(sys.argv[1]), raw(sys.argv[2])
if not src or len(src) != len(out):
    print('ERROR 尺寸或读取失败'); sys.exit(0)
# 用 magick 拿到尺寸
size = subprocess.run(['magick', 'identify', '-format', '%w %h', sys.argv[1]],
                      capture_output=True, text=True).stdout.split()
w, h = int(size[0]), int(size[1])
cols, rows = 4, 3
changed = 0
details = []
total_changed = 0
for r in range(rows):
    for c in range(cols):
        x0, x1 = w * c // cols, w * (c + 1) // cols
        y0, y1 = h * r // rows, h * (r + 1) // rows
        cell = 0
        for y in range(y0, y1, 2):          # 隔行采样，够用且快
            base = (y * w + x0) * 3
            for x in range(x0, x1, 2):
                i = base + (x - x0) * 3
                if src[i:i+3] != out[i:i+3]:
                    cell += 1
                    total_changed += 1
        details.append(cell)
        if cell > 0:
            changed += 1
print(f'CELLS={changed}/{cols*rows} TOTAL={total_changed} GRID={details}')
PY
)
echo "$GRID" > "$OUT/p2-grid.txt"
CELLS=$(echo "$GRID" | sed -nE 's/.*CELLS=([0-9]+)\/.*/\1/p')
if [ -n "${CELLS:-}" ] && [ "$CELLS" -ge 3 ]; then
    report "C. 标注覆盖多个区域" 1 "$GRID（4x3 网格中至少 3 格有变化）"
else
    report "C. 标注覆盖多个区域" 0 "$GRID"
fi

### D) 编辑器能打开，且未保存不写任何文件 -------------------------------------
SAVE_DIR=$(x=$(grep -m1 '保存目录:' "$OUT/a-selftest.txt" 2>/dev/null | sed 's/.*保存目录: //'); \
           echo "${x:-$HOME/Pictures/Screenshots}")
BEFORE=$(ls -1 "$SAVE_DIR" 2>/dev/null | wc -l)
timeout 6 "$SEASHOT" --edit "$OUT/p2-source.png" > "$OUT/p2-edit.txt" 2>&1
AFTER=$(ls -1 "$SAVE_DIR" 2>/dev/null | wc -l)
OPENED=0
grep -q '^EDITOR_OPEN' "$OUT/p2-edit.txt" && OPENED=1
if [ "$OPENED" = 1 ] && [ "$BEFORE" = "$AFTER" ]; then
    report "D. 编辑器打开且不留半成品" 1 "$(cat "$OUT/p2-edit.txt")；$SAVE_DIR 文件数 $BEFORE → $AFTER 未变"
else
    report "D. 编辑器打开且不留半成品" 0 \
        "打开=$OPENED；$SAVE_DIR 文件数 $BEFORE → $AFTER；输出：$(cat "$OUT/p2-edit.txt")"
fi

echo
echo "== 汇总: $PASS 通过 / $FAIL 失败 =="
echo "证据目录: $OUT"
[ "$FAIL" = 0 ]
