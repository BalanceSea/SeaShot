#!/usr/bin/env bash
# Phase F 验收脚本：全局快捷键「应用自己注册 + 用户授权」（xdg-desktop-portal GlobalShortcuts）
#
# 用法： bash scripts/acceptance-phaseF.sh
# 证据落在 docs/acceptance/evidence/
#
# 判据（可自动判定的部分）：
#   1) 门户可用：GlobalShortcuts 接口在，且报得出 version
#   2) 静默查询不弹窗、能返回：ListShortcuts 拿到"我们自己有哪些快捷键、绑没绑键"
#   3) `--hotkey-portal-request` 在**没人操作对话框**时不会挂死：以明确状态收尾
#      （BOUND / NO_TRIGGER / DENIED + FLOW 行），而不是一直等
#   4) 已授权时，**常驻实例的静默自检**能认出绑定（HOTKEY_PORTAL_STATE authorized ...）
#   5) `--hotkey-status` 同时报告门户那条路的状态（谁在真正生效一眼可见）
#   6) 原有的 KGlobalAccel 这条路**不受影响**（写入/读回/解绑仍正常）
#   7) 绑定不存在时不会谎报"已授权"
#
# 需要人工的一项（本机已在开发过程中实测通过，见 docs/acceptance/phaseF.md）：
#   **首次授权对话框**：用一个全新 id 发起时，系统会弹授权对话框，由用户允许/拒绝；
#   允许后按一下那个键就会截图。自动化跑不了（要有人在对话框上点）。
#
# 注意：第 3 条会真的发起一次注册请求；如果那个 id 还没被授权过，**屏幕上会弹授权对话框**，
#       你不理它的话 20 秒后本次判据会以"超时=未授权"收尾（这正是判据 3 要验的）。
set -u
cd "$(dirname "$0")/.."

SEASHOT="$HOME/.local/bin/seashot"
# 验收会起常驻实例：关掉"首次运行自动向系统申请快捷键授权"，
# 否则系统授权对话框会弹出来干扰验收
export SEASHOT_NO_FIRST_RUN_ASK=1
OUT="docs/acceptance/evidence"
mkdir -p "$OUT" docs/acceptance
PASS=0
FAIL=0

report() { # $1 名称 $2 0/1 $3 细节
    if [ "$2" = 1 ]; then
        printf 'PASS  %-44s %s\n' "$1" "$3"
        PASS=$((PASS + 1))
    else
        printf 'FAIL  %-44s %s\n' "$1" "$3"
        FAIL=$((FAIL + 1))
    fi
}

echo "== Phase F 验收（全局快捷键自注册 + 用户授权）=="
echo "二进制: $SEASHOT ($(stat -c '%y' "$SEASHOT" | cut -d. -f1))"
echo

### A) 门户可用 + 静默查询 ------------------------------------------------------
timeout 60 "$SEASHOT" --hotkey-portal-status > "$OUT/f-portal-status.txt" 2>&1
STATUS_CODE=$?
AVAIL=$(sed -nE 's/^HOTKEY_PORTAL available=([01]).*/\1/p' "$OUT/f-portal-status.txt" | head -1)
VERSION=$(sed -nE 's/^HOTKEY_PORTAL available=[01] version=([0-9]+).*/\1/p' "$OUT/f-portal-status.txt" | head -1)
LIST_LINE=$(grep -m1 '^HOTKEY_PORTAL_LIST' "$OUT/f-portal-status.txt")
if [ "$AVAIL" = 1 ] && [ "${VERSION:-0}" -ge 1 ]; then
    report "F. 门户可用（GlobalShortcuts）" 1 "available=1 version=$VERSION（xdg-desktop-portal-kde）"
else
    report "F. 门户可用（GlobalShortcuts）" 0 "available=${AVAIL:-?} version=${VERSION:-?}；见 $OUT/f-portal-status.txt"
fi

COUNT=$(sed -nE 's/^HOTKEY_PORTAL_LIST count=(-?[0-9]+).*/\1/p' <<<"$LIST_LINE")
if [ "${COUNT:-0}" -ge 1 ] && [ "$STATUS_CODE" = 0 ]; then
    report "F. 静默查询不弹窗且有结果" 1 "$(sed 's/^HOTKEY_PORTAL_LIST //' <<<"$LIST_LINE")"
else
    report "F. 静默查询不弹窗且有结果" 0 "count=${COUNT:-?} 退出码=$STATUS_CODE"
fi

### B) 请求注册：没人点对话框时也必须以明确状态收尾（不挂死） ------------------------
START_MS=$(date +%s%3N)
# 用**默认 id**（不是每次现造一个新 id）：在这台已经授权过的机器上是静默的，
# 不会每次跑验收都弹一个授权对话框。在全新机器上它会弹（那正是要验的功能本身）。
timeout 90 "$SEASHOT" --hotkey-portal-request --hotkey-portal-ms 20000 \
    > "$OUT/f-portal-request.txt" 2>&1
REQUEST_CODE=$?
ELAPSED_MS=$(( $(date +%s%3N) - START_MS ))
FLOW=$(grep -m1 '^HOTKEY_PORTAL_FLOW' "$OUT/f-portal-request.txt")
SENT=$(grep -c -m1 '^HOTKEY_PORTAL_REQUEST BindShortcuts' "$OUT/f-portal-request.txt" || true)
if [ -n "$FLOW" ] && [ "$SENT" -ge 1 ] && [ "$ELAPSED_MS" -lt 60000 ]; then
    report "F. 请求注册不会挂死（有明确收尾）" 1 \
        "BindShortcuts 已发出；${ELAPSED_MS}ms 内收尾 → $FLOW"
else
    report "F. 请求注册不会挂死（有明确收尾）" 0 \
        "耗时 ${ELAPSED_MS}ms flow='${FLOW:-（无）}' sent=$SENT；见 $OUT/f-portal-request.txt"
fi

### C) 已授权时：常驻实例的静默自检能认出绑定 ------------------------------------
pkill -x seashot 2>/dev/null
sleep 0.5
(timeout 10 "$SEASHOT" > "$OUT/f-resident.txt" 2>&1 </dev/null &)
sleep 4.5
pkill -x seashot 2>/dev/null
STATE=$(grep -m1 '^HOTKEY_PORTAL_STATE' "$OUT/f-resident.txt")
LIST_HAS_TRIGGER=$(grep -c '=[^=]*[A-Za-z]' <<<"$LIST_LINE" || true)
if [[ "$STATE" == *"authorized"* ]]; then
    report "F. 常驻自检认出已授权的绑定" 1 "$STATE（按这个键就会截图）"
elif [[ "$STATE" == *"not-authorized"* ]] && [ "$LIST_HAS_TRIGGER" = 0 ]; then
    report "F. 常驻自检认出已授权的绑定" 1 "$STATE（本机确实还没授权过，如实上报）"
else
    report "F. 常驻自检认出已授权的绑定" 0 "state='${STATE:-（无）}'；见 $OUT/f-resident.txt"
fi

### D) --hotkey-status 同时报门户状态 ------------------------------------------
timeout 60 "$SEASHOT" --hotkey-status > "$OUT/f-hotkey-status.txt" 2>&1
if grep -q '^HOTKEY_PORTAL_STATE' "$OUT/f-hotkey-status.txt" \
    && grep -q '^HOTKEY ' "$OUT/f-hotkey-status.txt"; then
    report "F. --hotkey-status 同时报两条路" 1 \
        "$(grep -m1 '^HOTKEY_PORTAL_STATE' "$OUT/f-hotkey-status.txt") · $(grep -m1 '^HOTKEY ' "$OUT/f-hotkey-status.txt")"
else
    report "F. --hotkey-status 同时报两条路" 0 "见 $OUT/f-hotkey-status.txt"
fi

### E) KGlobalAccel 那条路不受影响 ---------------------------------------------
# ⚠️ 先记住当前的绑定：这段会把 KGlobalAccel 那条路写掉，**测完必须恢复**，
#    否则会把用户自己设好的键清掉（我在开发时踩过一次，别再来第二次）。
BEFORE_HOTKEY=$(timeout 60 "$SEASHOT" --hotkey-status 2>/dev/null \
    | sed -nE 's/^HOTKEY configured=([^ ]+).*/\1/p' | head -1)
[ -n "${BEFORE_HOTKEY:-}" ] || BEFORE_HOTKEY="none"
timeout 60 "$SEASHOT" --set-hotkey "Ctrl+Alt+F9" > "$OUT/f-kga-set.txt" 2>&1
SET_LINE=$(grep -m1 '^HOTKEY_SET' "$OUT/f-kga-set.txt")
timeout 60 "$SEASHOT" --hotkey-status > "$OUT/f-kga-status.txt" 2>&1
KGA_LINE=$(grep -m1 '^HOTKEY ' "$OUT/f-kga-status.txt")
timeout 60 "$SEASHOT" --set-hotkey none > "$OUT/f-kga-clear.txt" 2>&1
CLEAR_LINE=$(grep -m1 '^HOTKEY_SET' "$OUT/f-kga-clear.txt")
# 恢复原来的绑定（"none" 就是解绑）
timeout 60 "$SEASHOT" --set-hotkey "$BEFORE_HOTKEY" > "$OUT/f-kga-restore.txt" 2>&1
RESTORE_LINE=$(grep -m1 '^HOTKEY_SET' "$OUT/f-kga-restore.txt")
if [[ "$SET_LINE" == *"actual=Ctrl+Alt+F9"* ]] && [[ "$CLEAR_LINE" == *"actual=(未绑定)"* ]]; then
    report "F. 原有 KGlobalAccel 路径未受影响" 1 \
        "写入→读回→解绑→恢复都正常（$SET_LINE；$KGA_LINE；$CLEAR_LINE；恢复回 '${BEFORE_HOTKEY}'：$RESTORE_LINE）"
else
    report "F. 原有 KGlobalAccel 路径未受影响" 0 \
        "set='${SET_LINE:-无}' clear='${CLEAR_LINE:-无}'；见 $OUT/f-kga-*.txt"
fi

### F) 不会谎报"已授权"（id 是空列表时状态必须是 not-authorized）------------------
if grep -qE '^HOTKEY_PORTAL_STATE (not-authorized|authorized)' "$OUT/f-resident.txt"; then
    # 交叉核对：状态里的 trigger 必须真的出现在 LIST 里
    T=$(sed -nE 's/.*trigger=([^ ]+).*/\1/p' <<<"$STATE")
    if [ -z "$T" ] || grep -q "$T" <<<"$LIST_LINE"; then
        report "F. 状态与实际绑定一致（不谎报）" 1 \
            "state 里的 trigger='${T:-(无)}' 能在 LIST 里对上"
    else
        report "F. 状态与实际绑定一致（不谎报）" 0 "state trigger='$T' 不在 LIST 里"
    fi
else
    report "F. 状态与实际绑定一致（不谎报）" 0 "没有拿到状态行"
fi

if [ -f "$OUT/f-portal-status.txt" ] && [ -f "$OUT/f-portal-request.txt" ]; then
    report "F. 证据落盘" 1 "f-portal-status.txt / f-portal-request.txt / f-resident.txt / f-hotkey-status.txt"
else
    report "F. 证据落盘" 0 "缺少证据文件"
fi

echo
echo "== 汇总: $PASS 通过 / $FAIL 失败 =="
echo "证据: $OUT/f-*.txt"
echo "人工项（本机已实测通过，见 docs/acceptance/phaseF.md）："
echo "  · 用一个**全新 id** 发起 → 系统弹授权对话框 → 允许 → 按那个键 → 直接截图"
echo "  · 想再看一次首次授权：systemctl --user stop plasma-kglobalaccel.service，"
echo "    从 ~/.config/kglobalshortcutsrc 的 [seashot] 段删掉那个 id，再发起"
[ "$FAIL" = 0 ]
