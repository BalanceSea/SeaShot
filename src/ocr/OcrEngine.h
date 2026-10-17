// OCR（Phase E）：把选区里的文字识别出来。
//
// 用系统的 `tesseract` 命令行，**图片走 stdin、文本走 stdout**：
// 不落中间文件、不用关心临时目录，也不依赖 tesseract 的 C++ API/开发包。
//
// 语言包是外部依赖：`tesseract --list-langs` 里得有 `chi_sim`（中文简体）与 `eng`（英文）。
// 缺了会明确报错（而不是静默返回空文本）。
#pragma once

#include <QImage>
#include <QObject>
#include <QString>
#include <QStringList>

#include <functional>

namespace seashot {

struct OcrResult {
    bool ok = false;
    QString text;        ///< 原始输出（保留换行）
    QStringList lines;   ///< 按行拆好（每行 trim 过；空行已去掉）
    QString error;
    QString languages;   ///< 实际用的语言串
    int psm = 3;
    qint64 elapsedMs = 0;

    QString summary() const;
};

class OcrEngine
{
public:
    /// tesseract 是否可用；languages 里返回本机装了的语言包
    static bool available(QStringList *languages = nullptr, QString *error = nullptr);
    /// 默认语言：中英都装了就用 "chi_sim+eng"，只有一个就用那个；都没有返回空
    static QString defaultLanguages(QString *error = nullptr);

    /// 同步识别（命令行/单测用）。languages 为空则用 defaultLanguages()
    static OcrResult recognize(const QImage &image, const QStringList &languages = {}, int psm = 3,
                               int timeoutMs = 60000);
    /// 异步识别（界面用，别卡住事件循环）
    static void recognizeAsync(const QImage &image, const QStringList &languages, int psm,
                               QObject *owner, std::function<void(const OcrResult &)> callback);
};

} // namespace seashot
