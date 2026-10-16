#include "backend/x11/Ewmh.h"

namespace seashot::ewmh {

namespace {
quint32 readCardinal(const QByteArray &data, int index)
{
    const int offset = index * 4;
    if (offset + 4 > data.size())
        return 0;
    const auto *bytes = reinterpret_cast<const unsigned char *>(data.constData() + offset);
    // X11 属性是主机字节序；本项目只面向 x86_64
    return quint32(bytes[0]) | (quint32(bytes[1]) << 8) | (quint32(bytes[2]) << 16)
        | (quint32(bytes[3]) << 24);
}
} // namespace

FrameExtents parseFrameExtents(const QByteArray &data, bool *ok)
{
    FrameExtents extents;
    const bool valid = data.size() >= 16;
    if (valid) {
        extents.left = int(readCardinal(data, 0));
        extents.right = int(readCardinal(data, 1));
        extents.top = int(readCardinal(data, 2));
        extents.bottom = int(readCardinal(data, 3));
    }
    if (ok)
        *ok = valid;
    return extents;
}

QVector<quint32> parseCardinalList(const QByteArray &data)
{
    QVector<quint32> values;
    const int count = data.size() / 4;
    values.reserve(count);
    for (int i = 0; i < count; ++i)
        values.append(readCardinal(data, i));
    return values;
}

QString parseUtf8String(const QByteArray &data)
{
    return QString::fromUtf8(data).trimmed();
}

QVector<quint32> parseAtomList(const QByteArray &data)
{
    return parseCardinalList(data);
}

bool atomListContains(const QByteArray &data, quint32 atom)
{
    if (atom == 0)
        return false;
    return parseAtomList(data).contains(atom);
}

int parsePid(const QByteArray &data)
{
    if (data.size() < 4)
        return 0;
    return int(readCardinal(data, 0));
}

QRectF frameRectFor(const QRect &clientGeometry, const FrameExtents &extents)
{
    return QRectF(clientGeometry.x() - extents.left, clientGeometry.y() - extents.top,
                  clientGeometry.width() + extents.left + extents.right,
                  clientGeometry.height() + extents.top + extents.bottom);
}

} // namespace seashot::ewmh
