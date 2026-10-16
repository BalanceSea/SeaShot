// EWMH/ICCCM 属性的纯解析函数（不依赖 xcb，便于单元测试）
#pragma once

#include <QByteArray>
#include <QRect>
#include <QRectF>
#include <QString>
#include <QVector>

namespace seashot::ewmh {

struct FrameExtents {
    int left = 0;
    int right = 0;
    int top = 0;
    int bottom = 0;
};

/// _NET_FRAME_EXTENTS：4 个 CARDINAL（left, right, top, bottom），32 位小端
FrameExtents parseFrameExtents(const QByteArray &data, bool *ok = nullptr);

/// _NET_CLIENT_LIST(_STACKING)：一串 32 位 XID，返回顺序即栈序（后面的越靠上）
QVector<quint32> parseCardinalList(const QByteArray &data);

/// _NET_WM_NAME / WM_NAME：UTF-8 字符串（长度由调用方按属性长度给出）
QString parseUtf8String(const QByteArray &data);

/// _NET_WM_STATE / _NET_WM_WINDOW_TYPE：一串 32 位 ATOM
QVector<quint32> parseAtomList(const QByteArray &data);
bool atomListContains(const QByteArray &data, quint32 atom);

/// _NET_WM_PID：单个 CARDINAL
int parsePid(const QByteArray &data);

/// 客户区矩形 + 外框扩展 → 窗口外框（含标题栏与边框），X11 下即“所见即所得”的范围
QRectF frameRectFor(const QRect &clientGeometry, const FrameExtents &extents);

} // namespace seashot::ewmh
