// --selftest：把 Phase 1 的验收前提逐条查一遍并打印结论
#pragma once

#include "backend/BackendFactory.h"

#include <QString>
#include <QVector>

namespace seashot {

class Settings;
class CaptureBackend;

class SelfTest
{
public:
    struct Check {
        QString name;
        bool ok = false;
        bool fatal = true; ///< false 表示只是提示（例如 X11 会话未实测）
        QString detail;
    };

    /// 逐条执行检查并打印；返回进程退出码（0 = 全部通过）
    static int run(CaptureBackend *backend, const BackendChoice &choice, Settings *settings,
                   bool x11Requested);
};

} // namespace seashot
