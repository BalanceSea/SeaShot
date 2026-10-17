// Exercise the shipped KWin script without desktop capture or live D-Bus.
const assert = require('node:assert/strict');
const fs = require('node:fs');
const vm = require('node:vm');
const signal = () => {
    const listeners = [];
    return { connect(fn) { listeners.push(fn); }, emit() { listeners.forEach(fn => fn()); } };
};
let nextId = 0;
const makeWindow = (appId, properties = {}) => ({
    desktopFileName: appId, internalId: String(++nextId),
    caption: '', normalWindow: true, visible: true,
    frameGeometry: { x: 0, y: 0, width: 100, height: 100 },
    clientGeometry: { x: 0, y: 0, width: 100, height: 100 },
    excludeFromCapture: false, captionChanged: signal(), ...properties,
});
const settings = makeWindow('seashot', { caption: 'SeaShot 设置', excludeFromCapture: true });
const menu = makeWindow('seashot', { normalWindow: false, popupMenu: true });
const editor = makeWindow('seashot', { caption: 'SeaShot 编辑器', excludeFromCapture: true });
const overlay = makeWindow('seashot', { normalWindow: false, onScreenDisplay: true });
const pin = makeWindow('seashot', { normalWindow: false, onScreenDisplay: true });
const dialog = makeWindow('seashot', { normalWindow: false, dialog: true });
const external = makeWindow('seashot-probe-window', { onScreenDisplay: true });
const windows = [settings, menu, editor, overlay, pin, dialog, external];
let snapshot;
const context = vm.createContext({
    workspace: {
        windowList: () => windows, stackingOrder: windows,
        windowAdded: signal(), windowRemoved: signal(), windowActivated: signal(),
    },
    QTimer: function () { this.timeout = signal(); this.start = () => {}; },
    callDBus(_service, _path, _iface, method, value) {
        if (method === 'Publish') snapshot = JSON.parse(value);
    },
    print() {},
});
vm.runInContext(fs.readFileSync(process.argv[2], 'utf8'), context);
vm.runInContext('handleCommand(JSON.stringify({action:"exclude-from-capture",value:true}))', context);
assert.equal(settings.excludeFromCapture, false, 'settings must be capturable after migration');
assert.equal(editor.excludeFromCapture, false, 'the editor is an ordinary capturable window');
assert.equal(menu.excludeFromCapture, false, 'a tray popup must remain capturable');
assert.equal(overlay.excludeFromCapture, true, 'the selection overlay must remain excluded');
assert.equal(pin.excludeFromCapture, true, 'pinned images must retain their exclusion policy');
assert.equal(external.excludeFromCapture, false, 'another application must not be changed');
vm.runInContext('lastSnapshot=""; publishSnapshot()', context);
assert.equal(snapshot.windows.find(w => w.id === dialog.internalId).normal, true, 'dialogs must be selectable');
assert.equal(snapshot.windows.find(w => w.id === menu.internalId).normal, true, 'menus must be selectable');
const added = makeWindow('seashot', { normalWindow: false, onScreenDisplay: true });
context.added = added;
vm.runInContext('watchWindow(added)', context);
assert.equal(added.excludeFromCapture, true, 'new overlays must also be excluded');
const renamed = makeWindow('seashot');
context.renamed = renamed;
vm.runInContext('watchWindow(renamed)', context);
assert.equal(renamed.excludeFromCapture, false);
renamed.caption = 'seashot-toast';
renamed.captionChanged.emit();
assert.equal(renamed.excludeFromCapture, true, 'late auxiliary titles must update exclusion');
renamed.caption = 'SeaShot 设置';
renamed.captionChanged.emit();
assert.equal(renamed.excludeFromCapture, false, 'ordinary windows must not keep stale flags');
vm.runInContext('handleCommand(JSON.stringify({action:"exclude-from-capture",value:false}))', context);
assert.equal(overlay.excludeFromCapture, false, 'diagnostic disabling of exclusion must work');
console.log('KWin exclusion policy and selectable dialog/menu regression checks passed');
