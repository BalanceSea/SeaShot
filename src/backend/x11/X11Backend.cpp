#include "backend/x11/X11Backend.h"

#include "backend/x11/Ewmh.h"
#include "util/Log.h"

#include <QGuiApplication>
#include <QScreen>

#include <cstdlib>
#include <cstring>
#include <sys/shm.h>

namespace seashot {

namespace {
// Qt::Key → X keysym 的**子集**映射（够用即可；X11 后端本期只做代码级+冒烟）
quint32 keysymForQtKey(int key)
{
    if (key >= Qt::Key_A && key <= Qt::Key_Z)
        return quint32(key); // 大写字母的 keysym 与其 ASCII 相同
    if (key >= Qt::Key_0 && key <= Qt::Key_9)
        return quint32(key);
    if (key >= Qt::Key_F1 && key <= Qt::Key_F35)
        return 0xffbe + quint32(key - Qt::Key_F1);
    switch (key) {
    case Qt::Key_Print: return 0xff61;
    case Qt::Key_Insert: return 0xff63;
    case Qt::Key_Delete: return 0xffff;
    case Qt::Key_Home: return 0xff50;
    case Qt::Key_End: return 0xff57;
    case Qt::Key_PageUp: return 0xff55;
    case Qt::Key_PageDown: return 0xff56;
    case Qt::Key_Left: return 0xff51;
    case Qt::Key_Up: return 0xff52;
    case Qt::Key_Right: return 0xff53;
    case Qt::Key_Down: return 0xff54;
    case Qt::Key_Space: return 0x0020;
    case Qt::Key_Return: return 0xff0d;
    case Qt::Key_Escape: return 0xff1b;
    case Qt::Key_Tab: return 0xff09;
    default: return 0;
    }
}
} // namespace

X11Backend::X11Backend() = default;

X11Backend::~X11Backend()
{
    shutdown();
}

bool X11Backend::initialize(QString *error)
{
    m_connection = xcb_connect(nullptr, nullptr);
    if (!m_connection || xcb_connection_has_error(m_connection)) {
        if (error)
            *error = QStringLiteral("无法连接 X11（DISPLAY=%1）")
                         .arg(QString::fromLocal8Bit(qgetenv("DISPLAY")));
        shutdown();
        return false;
    }

    xcb_intern_atom_cookie_t *cookies = xcb_ewmh_init_atoms(m_connection, &m_ewmh);
    if (!xcb_ewmh_init_atoms_replies(&m_ewmh, cookies, nullptr)) {
        if (error)
            *error = QStringLiteral("xcb_ewmh_init_atoms 失败");
        shutdown();
        return false;
    }

    const xcb_setup_t *setup = xcb_get_setup(m_connection);
    xcb_screen_iterator_t iterator = xcb_setup_roots_iterator(setup);
    m_screen = iterator.data;
    if (!m_screen) {
        if (error)
            *error = QStringLiteral("X11 没返回屏幕信息");
        shutdown();
        return false;
    }

    // MIT-SHM（实测：XWayland 下普通 GetImage 会返回 BadMatch code=8，ShmGetImage 正常）
    if (xcb_shm_query_version_reply_t *version =
            xcb_shm_query_version_reply(m_connection, xcb_shm_query_version(m_connection), nullptr)) {
        m_shmAvailable = true;
        SS_INFO() << "[x11] MIT-SHM 可用" << version->major_version << version->minor_version;
        std::free(version);
    } else {
        m_shmAvailable = false;
        SS_WARN() << "[x11] MIT-SHM 不可用，将退回普通 GetImage（在真实 X11 会话下可用）";
    }

    // X11 下逻辑坐标 == 物理像素（无合成器缩放）；输出信息仅用于诊断与多屏判断
    m_outputs.clear();
    for (const QScreen *screen : QGuiApplication::screens()) {
        OutputInfo info;
        info.name = screen->name();
        info.logicalGeometry = screen->geometry();
        info.scale = 1.0;
        m_outputs.append(info);
    }
    SS_INFO() << "[x11] 已连接，" << m_outputs.size() << "个输出";
    return true;
}

void X11Backend::shutdown()
{
    ungrabHotkey();
    releaseShm();
    if (m_connection) {
        xcb_disconnect(m_connection);
        m_connection = nullptr;
    }
}

void X11Backend::releaseShm()
{
    if (!m_connection && m_shmId < 0)
        return;
    if (m_connection && m_shmSeg) {
        xcb_shm_detach(m_connection, m_shmSeg);
        m_shmSeg = 0;
    }
    if (m_shmAddr) {
        ::shmdt(m_shmAddr);
        m_shmAddr = nullptr;
    }
    if (m_shmId >= 0) {
        ::shmctl(m_shmId, IPC_RMID, nullptr);
        m_shmId = -1;
    }
    m_shmSize = 0;
}

bool X11Backend::ensureShm(int bytes)
{
    if (!m_shmAvailable || !m_connection || bytes <= 0)
        return false;
    if (m_shmAddr && m_shmSize >= bytes)
        return true;

    releaseShm();
    m_shmId = ::shmget(IPC_PRIVATE, bytes, IPC_CREAT | 0600);
    if (m_shmId < 0) {
        SS_WARN() << "[x11] shmget 失败";
        return false;
    }
    m_shmAddr = ::shmat(m_shmId, nullptr, 0);
    if (m_shmAddr == reinterpret_cast<void *>(-1)) {
        m_shmAddr = nullptr;
        ::shmctl(m_shmId, IPC_RMID, nullptr);
        m_shmId = -1;
        SS_WARN() << "[x11] shmat 失败";
        return false;
    }
    ::memset(m_shmAddr, 0, size_t(bytes));
    m_shmSeg = xcb_generate_id(m_connection);
    xcb_shm_attach(m_connection, m_shmSeg, m_shmId, 0);
    m_shmSize = bytes;
    return true;
}

bool X11Backend::captureViaShm(const QRect &rect, CaptureImage *result)
{
    const int bytes = rect.width() * rect.height() * 4;
    if (!ensureShm(bytes))
        return false;

    xcb_generic_error_t *xerror = nullptr;
    xcb_shm_get_image_reply_t *reply = xcb_shm_get_image_reply(
        m_connection,
        xcb_shm_get_image(m_connection, m_screen->root, rect.x(), rect.y(),
                          quint16(rect.width()), quint16(rect.height()), ~0u,
                          XCB_IMAGE_FORMAT_Z_PIXMAP, m_shmSeg, 0),
        &xerror);
    if (!reply) {
        if (xerror) {
            SS_WARN() << "[x11] ShmGetImage 失败 code=" << xerror->error_code;
            std::free(xerror);
        }
        return false;
    }
    const int size = int(reply->size);
    std::free(reply);
    if (size < bytes) {
        SS_WARN() << "[x11] ShmGetImage 数据不足:" << size << "<" << bytes;
        return false;
    }
    result->image = QImage(reinterpret_cast<const uchar *>(m_shmAddr), rect.width(), rect.height(),
                           rect.width() * 4, QImage::Format_RGB32)
                        .copy();
    return true;
}

QByteArray X11Backend::getProperty(quint32 window, xcb_atom_t property, xcb_atom_t type, bool *ok) const
{
    QByteArray result;
    if (!m_connection) {
        if (ok)
            *ok = false;
        return result;
    }
    xcb_get_property_cookie_t cookie = xcb_get_property(m_connection, false, window, property, type, 0, 1024);
    xcb_get_property_reply_t *reply = xcb_get_property_reply(m_connection, cookie, nullptr);
    bool success = false;
    if (reply) {
        const int length = xcb_get_property_value_length(reply);
        if (length > 0) {
            result = QByteArray(static_cast<const char *>(xcb_get_property_value(reply)), length);
            success = true;
        }
        std::free(reply);
    }
    if (ok)
        *ok = success;
    return result;
}

QRect X11Backend::clientGeometryOf(quint32 window) const
{
    if (!m_connection)
        return {};
    xcb_get_geometry_reply_t *geometry = xcb_get_geometry_reply(m_connection, xcb_get_geometry(m_connection, window), nullptr);
    if (!geometry)
        return {};
    const QRect local(geometry->x, geometry->y, geometry->width, geometry->height);
    std::free(geometry);

    xcb_translate_coordinates_reply_t *translated = xcb_translate_coordinates_reply(
        m_connection, xcb_translate_coordinates(m_connection, window, m_screen->root, 0, 0), nullptr);
    if (!translated)
        return local;
    const int rootX = translated->dst_x;
    const int rootY = translated->dst_y;
    std::free(translated);
    return QRect(rootX, rootY, local.width(), local.height());
}

QVector<WindowInfo> X11Backend::collectWindows() const
{
    QVector<WindowInfo> windows;
    if (!m_connection)
        return windows;

    const QByteArray listData = getProperty(m_screen->root, m_ewmh._NET_CLIENT_LIST_STACKING, XCB_ATOM_WINDOW);
    QVector<quint32> ids = ewmh::parseCardinalList(listData);
    if (ids.isEmpty())
        ids = ewmh::parseCardinalList(getProperty(m_screen->root, m_ewmh._NET_CLIENT_LIST, XCB_ATOM_WINDOW));

    const QByteArray activeData = getProperty(m_screen->root, m_ewmh._NET_ACTIVE_WINDOW, XCB_ATOM_WINDOW);
    const quint32 activeId = ewmh::parseCardinalList(activeData).value(0, 0);

    int stackIndex = 0;
    for (quint32 id : ids) {
        if (id == 0)
            continue;
        WindowInfo info;
        info.id = QString::number(id, 16);
        info.stackIndex = stackIndex++;

        const QRect clientGeometry = clientGeometryOf(id);
        if (clientGeometry.isEmpty())
            continue;
        bool extentsOk = false;
        const auto extents = ewmh::parseFrameExtents(
            getProperty(id, m_ewmh._NET_FRAME_EXTENTS, XCB_ATOM_CARDINAL), &extentsOk);
        info.clientGeometry = clientGeometry;
        info.frameGeometry = extentsOk ? ewmh::frameRectFor(clientGeometry, extents)
                                       : QRectF(clientGeometry);

        info.title = ewmh::parseUtf8String(getProperty(id, m_ewmh._NET_WM_NAME, m_ewmh.UTF8_STRING));
        if (info.title.isEmpty())
            info.title = ewmh::parseUtf8String(getProperty(id, XCB_ATOM_WM_NAME, XCB_ATOM_STRING));
        info.pid = ewmh::parsePid(getProperty(id, m_ewmh._NET_WM_PID, XCB_ATOM_CARDINAL));
        // WM_CLASS：两个 NUL 分隔的字符串（instance\0class\0），取 class
        const QByteArray wmClass = getProperty(id, XCB_ATOM_WM_CLASS, XCB_ATOM_STRING);
        if (!wmClass.isEmpty()) {
            const QList<QByteArray> parts = wmClass.split('\0');
            info.appId = QString::fromLatin1(parts.value(parts.size() > 1 ? 1 : 0)).trimmed();
        }

        const QByteArray state = getProperty(id, m_ewmh._NET_WM_STATE, XCB_ATOM_ATOM);
        info.minimized = ewmh::atomListContains(state, m_ewmh._NET_WM_STATE_HIDDEN);
        const bool skipTaskbar = ewmh::atomListContains(state, m_ewmh._NET_WM_STATE_SKIP_TASKBAR);
        const bool skipPager = ewmh::atomListContains(state, m_ewmh._NET_WM_STATE_SKIP_PAGER);
        info.normal = !skipTaskbar && !skipPager;
        info.visible = !info.minimized;
        info.active = (id == activeId);
        windows.append(info);
    }

    // 栈序：_NET_CLIENT_LIST_STACKING 是自下而上，转成“越大越靠上”
    for (int i = 0; i < windows.size(); ++i)
        windows[i].stackIndex = i;
    return windows;
}

QVector<WindowInfo> X11Backend::windows() const
{
    return collectWindows();
}

void X11Backend::captureRect(const QRectF &logicalRect, ImageCallback callback)
{
    CaptureImage result;
    if (!m_connection) {
        result.error = QStringLiteral("X11 未连接");
        if (callback)
            callback(result);
        return;
    }
    const QRect rect = logicalRect.toAlignedRect().intersected(
        QRect(0, 0, m_screen->width_in_pixels, m_screen->height_in_pixels));
    if (rect.width() <= 0 || rect.height() <= 0) {
        result.error = QStringLiteral("请求的矩形为空或完全在屏幕外");
        if (callback)
            callback(result);
        return;
    }

    xcb_generic_error_t *xerror = nullptr;
    // 优先 MIT-SHM（实测 XWayland 下普通 GetImage 会 BadMatch）
    if (!captureViaShm(rect, &result)) {
        xcb_get_image_reply_t *reply = xcb_get_image_reply(
            m_connection,
            xcb_get_image(m_connection, XCB_IMAGE_FORMAT_Z_PIXMAP, m_screen->root, rect.x(), rect.y(),
                          quint16(rect.width()), quint16(rect.height()), ~0u),
            &xerror);
        if (!reply) {
            result.error =
                xerror
                ? QStringLiteral("XGetImage 失败：X 错误 code=%1 major=%2 rect=%3,%4 %5x%6 depth=%7 "
                                 "(MIT-SHM %8)")
                      .arg(xerror->error_code)
                      .arg(xerror->major_code)
                      .arg(rect.x())
                      .arg(rect.y())
                      .arg(rect.width())
                      .arg(rect.height())
                      .arg(m_screen->root_depth)
                      .arg(m_shmAvailable ? QStringLiteral("可用但失败") : QStringLiteral("不可用"))
                : QStringLiteral("XGetImage 失败（未收到 X 错误，可能是连接已断）");
            std::free(xerror);
            if (callback)
                callback(result);
            return;
        }
        const quint8 *data = xcb_get_image_data(reply);
        const int length = xcb_get_image_data_length(reply);
        const int stride = rect.width() * 4;
        if (length >= stride * rect.height())
            result.image = QImage(data, rect.width(), rect.height(), stride, QImage::Format_RGB32).copy();
        else
            result.error = QStringLiteral("XGetImage 数据不完整：%1 字节").arg(length);
        std::free(reply);
    }
    result.scale = 1.0;

    if (callback)
        callback(result);
}

void X11Backend::captureWindow(const WindowInfo &window, ImageCallback callback)
{
    // 限制（见头文件）：按 frame 矩形取屏幕像素，被遮挡时会截到遮挡物
    captureRect(window.frameGeometry, std::move(callback));
}

void X11Backend::ungrabHotkey()
{
    if (!m_connection || !m_hotkeyGrabbed)
        return;
    const quint32 variants[] = {0, XCB_MOD_MASK_LOCK, XCB_MOD_MASK_2, XCB_MOD_MASK_LOCK | XCB_MOD_MASK_2};
    for (quint32 extra : variants)
        xcb_ungrab_key(m_connection, xcb_keycode_t(m_hotkeyKeycode), m_screen->root,
                       quint16(m_hotkeyModifiers | extra));
    xcb_flush(m_connection);
    m_hotkeyGrabbed = false;
}

bool X11Backend::grabHotkey(const QKeySequence &sequence, QString *error)
{
    if (!m_connection) {
        if (error)
            *error = QStringLiteral("X11 未连接");
        return false;
    }
    if (sequence.isEmpty()) {
        ungrabHotkey();
        return true;
    }

    const int key = sequence[0].toCombined() & ~Qt::KeyboardModifierMask;
    const quint32 keysym = keysymForQtKey(key);
    if (keysym == 0) {
        if (error)
            *error = QStringLiteral("X11 后端暂不支持该按键（keysym 映射表未覆盖）");
        return false;
    }

    // 在键盘映射里找该 keysym 对应的 keycode
    const quint8 firstKeycode = xcb_get_setup(m_connection)->min_keycode;
    const quint8 keycodeCount = quint8(xcb_get_setup(m_connection)->max_keycode - firstKeycode + 1);
    xcb_get_keyboard_mapping_reply_t *mapping = xcb_get_keyboard_mapping_reply(
        m_connection, xcb_get_keyboard_mapping(m_connection, firstKeycode, keycodeCount), nullptr);
    if (!mapping) {
        if (error)
            *error = QStringLiteral("拿不到键盘映射");
        return false;
    }
    const xcb_keysym_t *syms = xcb_get_keyboard_mapping_keysyms(mapping);
    const int keysymCount = xcb_get_keyboard_mapping_keysyms_length(mapping);
    const quint32 keycode = findKeycode(std::span(syms, size_t(keysymCount)),
                                        mapping->keysyms_per_keycode, firstKeycode, keysym);
    std::free(mapping);
    if (keycode == 0) {
        if (error)
            *error = QStringLiteral("键盘映射里找不到该按键");
        return false;
    }

    quint32 modifiers = 0;
    const Qt::KeyboardModifiers mods = sequence[0].keyboardModifiers();
    if (mods & Qt::ShiftModifier)
        modifiers |= XCB_MOD_MASK_SHIFT;
    if (mods & Qt::ControlModifier)
        modifiers |= XCB_MOD_MASK_CONTROL;
    if (mods & Qt::AltModifier)
        modifiers |= XCB_MOD_MASK_1;
    if (mods & Qt::MetaModifier)
        modifiers |= XCB_MOD_MASK_4;

    ungrabHotkey();
    m_hotkeyKeycode = keycode;
    m_hotkeyModifiers = modifiers;
    const quint32 variants[] = {0, XCB_MOD_MASK_LOCK, XCB_MOD_MASK_2, XCB_MOD_MASK_LOCK | XCB_MOD_MASK_2};
    for (quint32 extra : variants) {
        xcb_grab_key(m_connection, true, m_screen->root, quint16(modifiers | extra),
                     xcb_keycode_t(keycode), XCB_GRAB_MODE_ASYNC, XCB_GRAB_MODE_ASYNC);
    }
    xcb_flush(m_connection);
    m_hotkeyGrabbed = true;
    return true;
}

quint32 X11Backend::findKeycode(std::span<const quint32> keysyms, int perKeycode,
                               quint8 firstKeycode, quint32 keysym)
{
    if (perKeycode <= 0 || keysym == 0)
        return 0;
    for (size_t i = 0; i < keysyms.size(); ++i) {
        if (keysyms[i] == keysym)
            return quint32(firstKeycode) + quint32(i / size_t(perKeycode));
    }
    return 0;
}

bool X11Backend::setHotkey(const QKeySequence &sequence, QString *error)
{
    return grabHotkey(sequence, error);
}

QString X11Backend::hotkeyStatus() const
{
    if (!m_hotkeyGrabbed)
        return QStringLiteral("(未绑定)");
    return QStringLiteral("keycode=%1 modifiers=0x%2")
        .arg(m_hotkeyKeycode)
        .arg(m_hotkeyModifiers, 0, 16);
}

bool X11Backend::nativeEventFilter(const QByteArray &eventType, void *message, qintptr *result)
{
    Q_UNUSED(result)
    if (!m_hotkeyGrabbed)
        return false;
    if (eventType != QByteArrayLiteral("xcb_generic_event_t"))
        return false;
    auto *event = static_cast<xcb_generic_event_t *>(message);
    if ((event->response_type & ~0x80) != XCB_KEY_PRESS)
        return false;
    auto *keyEvent = reinterpret_cast<xcb_key_press_event_t *>(event);
    if (keyEvent->detail != m_hotkeyKeycode)
        return false;
    if ((keyEvent->state & ~(XCB_MOD_MASK_LOCK | XCB_MOD_MASK_2)) != m_hotkeyModifiers)
        return false;
    if (m_hotkeyCallback)
        m_hotkeyCallback();
    return true;
}

QString X11Backend::limitations() const
{
    return QStringLiteral(
        "X11 后端：窗口枚举用 EWMH（_NET_CLIENT_LIST_STACKING/_NET_FRAME_EXTENTS），"
        "截图用 XGetImage，快捷键用 XGrabKey。**X11 会话未实测**（本机无 X11 会话），"
        "仅在 XWayland 下冒烟；XWayland 下只能看到并截取 X11 客户端窗口。"
        "窗口被遮挡时会截到遮挡物。");
}

QString X11Backend::diagnostics() const
{
    return QStringLiteral("后端: x11\nDISPLAY: %1\n连接: %2\nMIT-SHM: %3\n输出: %4\n%5")
        .arg(QString::fromLocal8Bit(qgetenv("DISPLAY")))
        .arg(m_connection ? QStringLiteral("已连接") : QStringLiteral("未连接"))
        .arg(m_shmAvailable ? QStringLiteral("可用") : QStringLiteral("不可用（退回 GetImage）"))
        .arg(m_outputs.size())
        .arg(limitations());
}

} // namespace seashot
