// 轻量日志（统一前缀，便于从终端与 journal 里筛）
#pragma once

#include <QDebug>

#define SS_INFO(...) qInfo(__VA_ARGS__)
#define SS_WARN(...) qWarning(__VA_ARGS__)
