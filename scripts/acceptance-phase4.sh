#!/usr/bin/env bash
# Phase 4 验收脚本：GIF 录制（只录选定区域 / 帧率可调 / 无上限 / 可停止）
#
# 用法： bash scripts/acceptance-phase4.sh
# 证据落在 docs/acceptance/evidence/
set -u
cd "$(dirname "$0")/.."

SEASHOT="$HOME/.local/bin/seashot"
OUT="docs/acceptance/evidence"
mkdir -p "$OUT" docs/acceptance
PASS=0
FAIL=0
SKIPPED=0

report() { # $1 名称 $2 0/1 $3 细节
    if [ "$2" = 1 ]; then
        printf 'PASS  %-36s %s\n' "$1" "$3"
        PASS=$((PASS + 1))
    else
        printf 'FAIL  %-36s %s\n' "$1" "$3"
        FAIL=$((FAIL + 1))
    fi
}

probe() { # $1 gif 路径，$2 字段
    ffprobe -v error -select_streams v:0 -show_entries "stream=$2" -of default=noprint_wrappers=1:nokey=1 "$1" 2>/dev/null | head -1
}

echo "== Phase 4 验收 =="
echo "二进制: $SEASHOT ($(stat -c '%y' "$SEASHOT" | cut -d. -f1))"
echo

### A) 单元测试（编码器命令/真编码、录制器帧率与丢帧策略） ----------------------
if ctest --test-dir build > "$OUT/p4-unit.txt" 2>&1; then
    report "A. 单测（编码器 + 录制器）" 1 "$(grep 'tests passed' "$OUT/p4-unit.txt" | tail -1)"
else
    report "A. 单测（编码器 + 录制器）" 0 "见 $OUT/p4-unit.txt"
fi

### B) 端到端：录 3 秒、默认 15fps -------------------------------------------------
pkill -x seashot 2>/dev/null; sleep 0.5
rm -f "$OUT/p4-basic.gif"
"$SEASHOT" --record --rect 300,200,320,240 --seconds 3 --save "$OUT/p4-basic.gif" \
    > "$OUT/p4-basic.txt" 2>&1
W=$(probe "$OUT/p4-basic.gif" width); H=$(probe "$OUT/p4-basic.gif" height)
RATE=$(probe "$OUT/p4-basic.gif" r_frame_rate); FRAMES=$(probe "$OUT/p4-basic.gif" nb_frames)
if [ "$W" = 320 ] && [ "$H" = 240 ] && [ "$RATE" = "15/1" ] && [ "${FRAMES:-0}" -ge 30 ]; then
    report "B. 3 秒录制（尺寸/帧率/帧数）" 1 "$(grep '^RECORD ok' "$OUT/p4-basic.txt") · GIF ${W}x${H} ${RATE} ${FRAMES} 帧"
else
    report "B. 3 秒录制（尺寸/帧率/帧数）" 0 "GIF ${W}x${H} ${RATE} ${FRAMES} 帧；日志见 $OUT/p4-basic.txt"
fi

### C) 指示器落在录制区域之外（不会被录进 GIF） -----------------------------------
IDX=$(grep '^RECORDING_INDICATOR_GEOMETRY' "$OUT/p4-basic.txt" | tail -1)
IY=$(echo "$IDX" | sed -nE 's/.* y=(-?[0-9]+) .*/\1/p')
IH=$(echo "$IDX" | sed -nE 's/.* h=([0-9]+) .*/\1/p')
RTOP=$(echo "$IDX" | sed -nE 's/.* regionTop=(-?[0-9]+) .*/\1/p')
RBOT=$(echo "$IDX" | sed -nE 's/.* regionBottom=(-?[0-9]+).*/\1/p')
if [ -n "${IY:-}" ] && { [ "$((IY + IH))" -le "${RTOP:-0}" ] || [ "$IY" -ge "${RBOT:-0}" ]; }; then
    report "C. 录制指示器在区域之外" 1 "$IDX"
else
    report "C. 录制指示器在区域之外" 0 "$IDX"
fi

### D) 帧率可调 -----------------------------------------------------------------
kwriteconfig6 --file seashot.conf --group record --key fps 10 2>/dev/null
rm -f "$OUT/p4-fps10.gif"
"$SEASHOT" --record --rect 300,200,240,180 --seconds 2 --save "$OUT/p4-fps10.gif" \
    > "$OUT/p4-fps10.txt" 2>&1
RATE10=$(probe "$OUT/p4-fps10.gif" r_frame_rate)
kwriteconfig6 --file seashot.conf --group record --key fps 15 2>/dev/null
if [ "$RATE10" = "10/1" ]; then
    report "D. 帧率可调（10fps）" 1 "$(grep '^RECORD ok' "$OUT/p4-fps10.txt")"
else
    report "D. 帧率可调（10fps）" 0 "GIF r_frame_rate=$RATE10（期望 10/1）"
fi

### E) 无时长上限（录 8 秒：帧数应当约为 3 秒的 8/3 倍） ----------------------------
# ⚠️ 判据只看**帧数/时长**，不比文件体积：GIF 体积取决于**画面内容**
#    （录一段不动的画面能小到几十 KB，动起来就大得多）—— 拿体积比会在桌面动/不动之间假红，
#    实测踩过：8 秒只有 53797B 而 3 秒有 136221B，纯粹因为那 8 秒画面更"静"。
rm -f "$OUT/p4-long.gif"
"$SEASHOT" --record --rect 300,200,320,240 --seconds 8 --save "$OUT/p4-long.gif" \
    > "$OUT/p4-long.txt" 2>&1
LONG_FRAMES=$(probe "$OUT/p4-long.gif" nb_frames)
SHORT_FRAMES=$(probe "$OUT/p4-basic.gif" nb_frames)
LONG_SIZE=$(stat -c%s "$OUT/p4-long.gif" 2>/dev/null || echo 0)
SHORT_SIZE=$(stat -c%s "$OUT/p4-basic.gif" 2>/dev/null || echo 0)
LONG_MS=$(sed -nE 's/^RECORD ok .*elapsedMs=([0-9]+).*/\1/p' "$OUT/p4-long.txt" | tail -1)
if [ "${LONG_FRAMES:-0}" -ge 100 ] && [ "${SHORT_FRAMES:-0}" -gt 0 ] \
    && [ "$LONG_FRAMES" -ge $((SHORT_FRAMES * 2)) ] && [ "${LONG_MS:-0}" -ge 7000 ]; then
    report "E. 无时长上限（8 秒）" 1 \
        "$(grep '^RECORD ok' "$OUT/p4-long.txt" | sed 's/ path=[^ ]*//') · 帧数 ${LONG_FRAMES} vs 3 秒的 ${SHORT_FRAMES}（体积仅供参考：${LONG_SIZE}B vs ${SHORT_SIZE}B）"
else
    report "E. 无时长上限（8 秒）" 0 \
        "8s=${LONG_FRAMES} 帧/${LONG_MS}ms，3s=${SHORT_FRAMES} 帧（要求 8s 帧数 ≥ 3s 的两倍且耗时 ≥7s）"
fi

### F) 停止路径：常驻实例 + 实例间转发（托盘/快捷键走同一条 stopRecording） --------
# 注意：交互式录制（没给 --save）现在是**延迟导出** —— 先只收尾无损中间文件，
# 再由结果浮层上的「导出 GIF / 导出 MP4 / 复制 GIF」按钮决定格式（ISSUE-010 那一轮的行为）。
# 所以这里不能再用 "RECORD ok path= 非空" 当判据：
#   1) 转发=1、停止=1、`RECORD ok` 出现；
#   2) 确实是延迟导出（`path=` 为空 + 日志里有"等用户选导出格式"）；
#   3) 「保存/导出」窗口里的按钮真能把格式定下来 —— 用 SEASHOT_EXPORT_AUTOCLICK=gif
#      让它自动点「导出 GIF」（产品里结果浮层已退役，录制结束会自动打开这个窗口）
#      （走 Toast::activate()，与真实鼠标点击同一条路径），最后验 GIF 落盘。
pkill -x seashot 2>/dev/null; sleep 0.5
SEASHOT_EXPORT_AUTOCLICK=gif "$SEASHOT" > "$OUT/p4-resident.txt" 2>&1 &
sleep 2.5
FWD=$("$SEASHOT" --record --rect 300,200,256,192 2>&1 | grep -c '^RECORD_FORWARDED' || true)
sleep 2
STOP=$(timeout 20 "$SEASHOT" --record-stop 2>&1 | grep -c '^RECORD_STOP_FORWARDED' || true)
# 等自动点导出 + ffmpeg 转码（同步阻塞，30 帧量级几秒）
for _ in $(seq 1 20); do
    grep -q '^RECORD_EXPORT' "$OUT/p4-resident.txt" && break
    sleep 1
done
pkill -x seashot 2>/dev/null; sleep 0.5
RESIDENT_OK=$(grep -c '^RECORD ok' "$OUT/p4-resident.txt" || true)
DEFERRED_PATH=$(sed -nE 's/^RECORD ok path=([^ ]*).*/\1/p' "$OUT/p4-resident.txt" | tail -1)
DEFER_MARK=$(grep -c '等用户选导出格式' "$OUT/p4-resident.txt" || true)
EXPORT_OK=$(grep -c '^RECORD_EXPORT ok format=gif' "$OUT/p4-resident.txt" || true)
GIF_PATH=$(sed -nE 's/^RECORD_EXPORT ok .*path=([^ ]+).*/\1/p' "$OUT/p4-resident.txt" | tail -1)
if [ "$FWD" = 1 ] && [ "$STOP" = 1 ] && [ "$RESIDENT_OK" = 1 ] && [ -z "${DEFERRED_PATH:-}" ] \
    && [ "$DEFER_MARK" = 1 ] && [ "$EXPORT_OK" = 1 ] && [ -f "$GIF_PATH" ]; then
    report "F. 常驻实例录制 + 转发停止" 1 \
        "转发=$FWD 停止=$STOP → 延迟导出（浮层选格式）+ 自动点「导出 GIF」→ $(grep '^RECORD_EXPORT ok' "$OUT/p4-resident.txt" | tail -1 | sed 's|path=.*/|path=…/|')  $(ffprobe -v error -select_streams v:0 -show_entries stream=nb_frames -of default=noprint_wrappers=1:nokey=1 "$GIF_PATH" 2>/dev/null) 帧"
    rm -f "$GIF_PATH"
else
    report "F. 常驻实例录制 + 转发停止" 0 \
        "转发=$FWD 停止=$STOP RECORD_ok=$RESIDENT_OK 延迟导出标记=$DEFER_MARK path='${DEFERRED_PATH:-}' 导出=$EXPORT_OK gif='${GIF_PATH:-}'；见 $OUT/p4-resident.txt"
fi

### G) 中间文件必须清理（位置见 ISSUE-011：~/.cache/seashot/recording；旧路径 /tmp 一并查）
LEFT_TMP=$(find /tmp -maxdepth 1 -name 'seashot-record-*.mkv' 2>/dev/null | wc -l)
LEFT_CACHE=$(find "$HOME/.cache/seashot/recording" -maxdepth 1 -name 'seashot-record-*' 2>/dev/null | wc -l)
if [ "$LEFT_TMP" = 0 ] && [ "$LEFT_CACHE" = 0 ]; then
    report "G. FFV1 中间文件已清理" 1 "/tmp 与 ~/.cache/seashot/recording 都没有残留"
else
    report "G. FFV1 中间文件已清理" 0 "/tmp 残留 $LEFT_TMP 个，~/.cache/seashot/recording 残留 $LEFT_CACHE 个"
fi

echo
echo "== 汇总: $PASS 通过 / $FAIL 失败 / $SKIPPED 跳过 =="
echo "证据目录: $OUT"
[ "$FAIL" = 0 ]
