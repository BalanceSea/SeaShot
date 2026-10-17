// SeaShot Phase 0 探测脚本（只读）
// 目的：确认 KWin 脚本能否稳定提供窗口列表、frameGeometry、clientGeometry
// 与可用于 org.kde.KWin.ScreenShot2.CaptureWindow 的标识符（internalId）。
// 本脚本不修改任何窗口状态，只把 JSON 打到 KWin 日志（journal）里。
// 加载方式：
//   qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.loadScript \
//       "$PWD/probes/kwin-probe.js" "seashot-probe0"
//   qdbus6 org.kde.KWin /Scripting org.kde.kwin.Scripting.start

function rectOf(getter) {
    try {
        const g = getter();
        if (!g)
            return null;
        return { x: g.x, y: g.y, w: g.width, h: g.height };
    } catch (e) {
        return "ERR:" + e;
    }
}

function probe() {
    const all = workspace.windowList();
    const out = [];
    for (let i = 0; i < all.length; i++) {
        const w = all[i];
        if (!w || w.deleted)
            continue;
        let uuid = "";
        try { uuid = String(w.internalId); } catch (e) { uuid = "ERR:" + e; }
        let pid = 0;
        try { pid = Number(w.pid); } catch (e) {}
        let res = "";
        try { res = String(w.resourceClass || w.resourceName || ""); } catch (e) {}
        let caption = "";
        try { caption = String(w.caption || ""); } catch (e) {}
        let active = false;
        try { active = !!w.active; } catch (e) {}
        let normal = false;
        try { normal = !!w.normalWindow; } catch (e) {}
        out.push({
            uuid: uuid,
            pid: pid,
            resource: res,
            caption: caption,
            active: active,
            normalWindow: normal,
            frame: rectOf(function () { return w.frameGeometry; }),
            client: rectOf(function () { return w.clientGeometry; })
        });
    }
    print("SEASHOT_PROBE_WINDOWS " + JSON.stringify(out));
    print("SEASHOT_PROBE_COUNT " + out.length);
}

probe();
