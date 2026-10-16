// 轻量日志：既写 stderr（终端直接可见），也保留 QDebug 的排版习惯
//
// 说明：Qt 默认会过滤掉 qInfo —— 本机实测 stderr 一片空白，而 Wayland/KWin 的失败
// 往往是「看不见的静默失败」，所以这里显式落到 stderr。
// 用法兼容两种：SS_INFO() << "文本"; 与 SS_INFO("文本");
#pragma once

#include <QDebug>
#include <QString>

#include <cstdio>

namespace seashot {

inline void logLine(const char *level, const QString &message)
{
    std::fprintf(stderr, "[%s] %s\n", level, qUtf8Printable(message));
    std::fflush(stderr);
}

class LogStream
{
public:
    explicit LogStream(const char *level)
        : m_level(level)
        , m_debug(&m_message)
    {
    }

    ~LogStream() { logLine(m_level, m_message); }

    template <typename T>
    LogStream &operator<<(const T &value)
    {
        m_debug << value;
        return *this;
    }

private:
    const char *m_level;
    QString m_message;
    QDebug m_debug;
};

} // namespace seashot

#define SS_INFO(...) seashot::LogStream("info") __VA_OPT__(<<) __VA_ARGS__
#define SS_WARN(...) seashot::LogStream("warn") __VA_OPT__(<<) __VA_ARGS__
