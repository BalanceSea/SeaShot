// SeaShot 的 KWin 助手脚本（Phase 1）
//
// 职责：
//   ① 把窗口快照（id/几何/栈序/状态）推给 seashot 进程；
//   ② 只把 SeaShot 的选框、贴图、提示层和录制指示器排除出截图；保留普通窗口
//      （Phase 0 实测：layer-shell 覆盖层会被 CaptureArea 拍到，
//       而 w.excludeFromCapture = true 能让它从截图里消失）。
//
// 由 seashot 通过 org.kde.kwin.Scripting.loadScript 加载，退出时卸载。
// 只读窗口状态 + 只改我们自己窗口的 excludeFromCapture，不碰用户窗口布局。

const SERVICE = "__SEASHOT_SERVICE__"; // 由 SeaShot 写入脚本时替换为 org.seashot.SeaShot.p<pid>
const PATH = "/KWinBridge";
const IFACE = "org.seashot.KWinBridge";

var lastSnapshot = "";

// 自排除的持续状态：不仅要给「当时已存在」的窗口打标记，新窗口（例如
// 刚创建、还没显示的覆盖层）也必须自动带上，否则第一次截图会把覆盖层拍进去。
var excludeState = { active: false, match: "seashot", value: true };

function appIdOfLoose(w) {
    try {
        return String(w.desktopFileName || w.resourceClass || w.resourceName || "");
    } catch (e) {
        return "";
    }
}

function isOwnWindow(window) {
    // 只匹配 appId 完全等于 "seashot"（或 "seashot." 前缀），**不做子串匹配**：
    // 否则 "seashot-probe-window" 这类第三方窗口会被误标成排除出截图，
    // 而 KWin 对已排除的窗口会直接取消截图（实测 Error.Cancelled）。
    const appId = appIdOfLoose(window);
    return appId === excludeState.match || appId.indexOf(excludeState.match + ".") === 0;
}

function isCaptureOverlay(window) {
    if (!window || !isOwnWindow(window))
        return false;
    // The auxiliary layer-shell surfaces use namespace "on-screen-display".
    // Named titles also cover the non-Wayland fallback, without excluding menus.
    const title = String(window.caption || "");
    return boolOf(window, "onScreenDisplay", false)
        || ["seashot-overlay", "seashot-pin", "seashot-toast", "seashot-recording"].indexOf(title) >= 0;
}

function applyExcludeTo(window) {
    if (!excludeState.active || !window || window.deleted || !isOwnWindow(window))
        return false;
    const excluded = excludeState.value && isCaptureOverlay(window);
    try {
        // Explicitly clear old blanket exclusion on settings, editors and menus.
        if (!!window.excludeFromCapture !== excluded)
            window.excludeFromCapture = excluded;
        return true;
    } catch (e) {
        print("[seashot] 设置 excludeFromCapture 失败: " + e);
        return false;
    }
}

function rectOf(getter) {
    try {
        const g = getter();
        if (!g)
            return null;
        return { x: g.x, y: g.y, width: g.width, height: g.height };
    } catch (e) {
        return null;
    }
}

function boolOf(w, name, fallback) {
    try {
        const value = w[name];
        return (value === undefined || value === null) ? fallback : !!value;
    } catch (e) {
        return fallback;
    }
}

function appIdOf(w) {
    try {
        return String(w.desktopFileName || w.resourceClass || w.resourceName || "");
    } catch (e) {
        return "";
    }
}

// workspace.stackingOrder 在部分 KWin 版本不暴露 → 退回 -1（调用侧按列表顺序兜底）
function stackIndexMap() {
    const map = {};
    try {
        const order = workspace.stackingOrder;
        if (order && order.length) {
            for (let i = 0; i < order.length; i++) {
                const w = order[i];
                if (w && w.internalId)
                    map[String(w.internalId)] = i;
            }
        }
    } catch (e) {
    }
    return map;
}

function publishSnapshot() {
    const list = workspace.windowList();
    const stack = stackIndexMap();
    const windows = [];
    for (let i = 0; i < list.length; i++) {
        const w = list[i];
        if (!w || w.deleted)
            continue;
        let id = "";
        try { id = String(w.internalId); } catch (e) { id = ""; }
        if (!id)
            continue;
        let pid = 0;
        try { pid = Number(w.pid); } catch (e) { pid = 0; }
        let title = "";
        try { title = String(w.caption || ""); } catch (e) { title = ""; }
        let active = false;
        try { active = !!w.active; } catch (e) { active = false; }
        let excluded = false;
        try { excluded = !!w.excludeFromCapture; } catch (e) { excluded = false; }
        windows.push({
            id: id,
            pid: pid,
            appId: appIdOf(w),
            title: title,
            frame: rectOf(function () { return w.frameGeometry; }),
            client: rectOf(function () { return w.clientGeometry; }),
            stackIndex: (stack[id] === undefined ? -1 : stack[id]),
            active: active,
            minimized: boolOf(w, "minimized", false),
            normal: boolOf(w, "normalWindow", false) || boolOf(w, "dialog", false)
                || boolOf(w, "popupMenu", false) || boolOf(w, "dropdownMenu", false)
                || boolOf(w, "appletPopup", false),
            visible: boolOf(w, "visible", true),
            excludedFromCapture: excluded,
            captureOverlay: isCaptureOverlay(w)
        });
    }
    const json = JSON.stringify({ windows: windows });
    if (json === lastSnapshot)
        return;
    lastSnapshot = json;
    callDBus(SERVICE, PATH, IFACE, "Publish", json);
}

const snapshotTimer = new QTimer();
snapshotTimer.interval = 120;
snapshotTimer.singleShot = true;
snapshotTimer.timeout.connect(publishSnapshot);

function scheduleSnapshot() {
    snapshotTimer.start();
}

function handleCommand(text) {
    if (!text)
        return;
    let cmd = null;
    try {
        cmd = JSON.parse(text);
    } catch (e) {
        print("[seashot] 命令解析失败: " + e);
        return;
    }
    if (cmd.action === "exclude-from-capture") {
        excludeState.active = true;
        excludeState.match = String(cmd.match || "seashot");
        excludeState.value = cmd.value !== false;
        const list = workspace.windowList();
        let changed = 0;
        for (let i = 0; i < list.length; i++) {
            if (applyExcludeTo(list[i]))
                changed++;
        }
        print("[seashot] exclude-from-capture value=" + excludeState.value + " changed=" + changed);
        // 强制重发快照，调用方才能读回标志做验收
        lastSnapshot = "";
        scheduleSnapshot();
        return;
    }
    print("[seashot] 未知命令: " + cmd.action);
}

const commandTimer = new QTimer();
commandTimer.interval = 50;
commandTimer.singleShot = false;
let pollInFlight = false;
commandTimer.timeout.connect(function () {
    if (pollInFlight)
        return;
    pollInFlight = true;
    callDBus(SERVICE, PATH, IFACE, "TakeCommand", function (text) {
        pollInFlight = false;
        if (text)
            handleCommand(text);
    });
});
commandTimer.start();

function watchWindow(w) {
    if (!w)
        return;
    // 新出现的窗口（含我们自己的覆盖层/浮层）也要自动带上自排除标记
    applyExcludeTo(w);
    const hook = function () { applyExcludeTo(w); scheduleSnapshot(); };
    const names = ["captionChanged", "desktopFileNameChanged", "activeChanged", "minimizedChanged",
                   "fullScreenChanged", "skipTaskbarChanged", "maximizedChanged",
                   "frameGeometryChanged", "excludeFromCaptureChanged"];
    for (let i = 0; i < names.length; i++) {
        try {
            if (w[names[i]])
                w[names[i]].connect(hook);
        } catch (e) {
        }
    }
}

const initial = workspace.windowList();
for (let i = 0; i < initial.length; i++)
    watchWindow(initial[i]);

workspace.windowAdded.connect(function (w) {
    watchWindow(w);
    scheduleSnapshot();
});
workspace.windowRemoved.connect(scheduleSnapshot);
workspace.windowActivated.connect(scheduleSnapshot);

// 心跳：悬停命中需要足够新鲜的快照，1s 兜底重发一次
const heartbeat = new QTimer();
heartbeat.interval = 1000;
heartbeat.singleShot = false;
heartbeat.timeout.connect(function () {
    lastSnapshot = "";
    publishSnapshot();
});
heartbeat.start();

publishSnapshot();
print("[seashot] KWin 助手脚本已加载");
