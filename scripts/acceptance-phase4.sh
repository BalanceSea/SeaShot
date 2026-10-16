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

### E) 无时长/体积上限（录 8 秒，帧数与体积都应明显更大） -------------------------
rm -f "$OUT/p4-long.gif"
"$SEASHOT" --record --rect 300,200,320,240 --seconds 8 --save "$OUT/p4-long.gif" \
    > "$OUT/p4-long.txt" 2>&1
LONG_FRAMES=$(probe "$OUT/p4-long.gif" nb_frames)
SHORT_FRAMES=$(probe "$OUT/p4-basic.gif" nb_frames)
LONG_SIZE=$(stat -c%s "$OUT/p4-long.gif" 2>/dev/null || echo 0)
SHORT_SIZE=$(stat -c%s "$OUT/p4-basic.gif" 2>/dev/null || echo 0)
if [ "${LONG_FRAMES:-0}" -ge 100 ] && [ "$LONG_FRAMES" -gt "$SHORT_FRAMES" ] \
    && [ "$LONG_SIZE" -gt "$SHORT_SIZE" ]; then
    report "E. 无时长/体积上限（8 秒）" 1 "$(grep '^RECORD ok' "$OUT/p4-long.txt") · ${LONG_SIZE} 字节 vs 3 秒的 ${SHORT_SIZE} 字节"
else
    report "E. 无时长/体积上限（8 秒）" 0 "8s=${LONG_FRAMES} 帧/${LONG_SIZE}B，3s=${SHORT_FRAMES} 帧/${SHORT_SIZE}B"
fi

### F) 停止路径：常驻实例 + 实例间转发（托盘/快捷键走同一条 stopRecording） --------
pkill -x seashot 2>/dev/null; sleep 0.5
( "$SEASHOT" > "$OUT/p4-resident.txt" 2>&1 ) &
sleep 2.5
FWD=$("$SEASHOT" --record --rect 300,200,256,192 2>&1 | grep -c '^RECORD_FORWARDED' || true)
sleep 2
STOP=$(timeout 20 "$SEASHOT" --record-stop 2>&1 | grep -c '^RECORD_STOP_FORWARDED' || true)
sleep 3
pkill -x seashot 2>/dev/null
RESIDENT_OK=$(grep -c '^RECORD ok' "$OUT/p4-resident.txt" || true)
GIF_PATH=$(sed -nE 's/^RECORD ok path=([^ ]+).*/\1/p' "$OUT/p4-resident.txt" | tail -1)
if [ "$FWD" = 1 ] && [ "$STOP" = 1 ] && [ "$RESIDENT_OK" = 1 ] && [ -f "$GIF_PATH" ]; then
    report "F. 常驻实例录制 + 转发停止" 1 "转发=$FWD 停止=$STOP → $(grep '^RECORD ok' "$OUT/p4-resident.txt" | tail -1)"
    rm -f "$GIF_PATH"
else
    report "F. 常驻实例录制 + 转发停止" 0 "转发=$FWD 停止=$STOP RECORD_ok=$RESIDENT_OK path='${GIF_PATH:-}'；见 $OUT/p4-resident.txt"
fi

### G) 中间文件必须清理 ---------------------------------------------------------
LEFT=$(find /tmp -maxdepth 1 -name 'seashot-record-*.mkv' 2>/dev/null | wc -l)
if [ "$LEFT" = 0 ]; then
    report "G. FFV1 中间文件已清理" 1 "/tmp 下没有 seashot-record-*.mkv"
else
    report "G. FFV1 中间文件已清理" 0 "残留 $LEFT 个中间文件"
fi

echo
echo "== 汇总: $PASS 通过 / $FAIL 失败 / $SKIPPED 跳过 =="
echo "证据目录: $OUT"
[ "$FAIL" = 0 ]
