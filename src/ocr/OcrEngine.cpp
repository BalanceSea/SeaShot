#include "ocr/OcrEngine.h"

#include "util/Log.h"

#include <QBuffer>
#include <QElapsedTimer>
#include <QProcess>
#include <QRegularExpression>

namespace seashot {

namespace {
QString tesseractBinary()
{
    const QString override = qEnvironmentVariable("SEASHOT_TESSERACT");
    return override.isEmpty() ? QStringLiteral("tesseract") : override;
}

/// 把 QImage 编成 PNG 字节（tesseract 从 stdin 读，格式靠内容识别）
QByteArray encodePng(const QImage &image)
{
    QByteArray bytes;
    QBuffer buffer(&bytes);
    buffer.open(QIODevice::WriteOnly);
    // 灰度化 + 放大一点点对 OCR 更友好；但**不改尺寸**，避免坐标/清晰度意外变化
    image.convertToFormat(QImage::Format_RGB32).save(&buffer, "PNG");
    return bytes;
}

QStringList parseLines(const QString &text)
{
    QStringList lines;
    for (const QString &raw : text.split(QLatin1Char('\n'))) {
        const QString line = raw.trimmed();
        if (!line.isEmpty())
            lines << line;
    }
    return lines;
}
} // namespace

QString OcrResult::summary() const
{
    if (!ok)
        return QStringLiteral("失败：%1").arg(error);
    return QStringLiteral("语言=%1 psm=%2 行数=%3 耗时=%4ms")
        .arg(languages)
        .arg(psm)
        .arg(lines.size())
        .arg(elapsedMs);
}

bool OcrEngine::available(QStringList *languages, QString *error)
{
    QProcess process;
    process.start(tesseractBinary(), {QStringLiteral("--list-langs")});
    if (!process.waitForFinished(10000) || process.exitCode() != 0) {
        if (error)
            *error = QStringLiteral("找不到可用的 tesseract（装了没？：pacman -S tesseract tesseract-data-chi_sim tesseract-data-eng）");
        return false;
    }
    if (languages) {
        languages->clear();
        const QString output = QString::fromLocal8Bit(process.readAllStandardOutput());
        const QRegularExpression re(QStringLiteral("^([a-z_]+)$"), QRegularExpression::MultilineOption);
        auto it = re.globalMatch(output);
        while (it.hasNext())
            languages->append(it.next().captured(1));
    }
    if (error)
        error->clear();
    return true;
}

QString OcrEngine::defaultLanguages(QString *error)
{
    QStringList languages;
    QString listError;
    if (!available(&languages, &listError)) {
        if (error)
            *error = listError;
        return QString();
    }
    QStringList wanted;
    if (languages.contains(QLatin1String("chi_sim")))
        wanted << QStringLiteral("chi_sim");
    if (languages.contains(QLatin1String("eng")))
        wanted << QStringLiteral("eng");
    if (wanted.isEmpty()) {
        if (error)
            *error = QStringLiteral("缺少语言包：需要 chi_sim / eng（pacman -S tesseract-data-chi_sim tesseract-data-eng），"
                                    "本机现有：%1")
                         .arg(languages.join(QLatin1Char(',')));
        return QString();
    }
    if (error)
        error->clear();
    return wanted.join(QLatin1Char('+'));
}

void OcrEngine::recognizeAsync(const QImage &image, const QStringList &languages, int psm,
                               QObject *owner, std::function<void(const OcrResult &)> callback)
{
    if (image.isNull()) {
        OcrResult result;
        result.error = QStringLiteral("空图像");
        callback(result);
        return;
    }
    QStringList effective = languages;
    if (effective.isEmpty()) {
        QString languageError;
        const QString resolved = defaultLanguages(&languageError);
        if (resolved.isEmpty()) {
            OcrResult result;
            result.error = languageError;
            callback(result);
            return;
        }
        effective = resolved.split(QLatin1Char('+'));
    }

    auto *process = new QProcess(owner);
    const QString languageArg = effective.join(QLatin1Char('+'));
    const QStringList args{QStringLiteral("-"), QStringLiteral("-"), QStringLiteral("-l"),
                           languageArg, QStringLiteral("--psm"), QString::number(psm)};
    QElapsedTimer timer;
    timer.start();

    QObject::connect(process, &QProcess::errorOccurred, owner, [process, callback](QProcess::ProcessError) {
        OcrResult result;
        result.error = QStringLiteral("tesseract 启动失败：%1").arg(process->errorString());
        callback(result);
        process->deleteLater();
    });
    QObject::connect(process,
                     QOverload<int, QProcess::ExitStatus>::of(&QProcess::finished), owner,
                     [process, callback, timer, languageArg, psm](int code, QProcess::ExitStatus status) {
                         OcrResult result;
                         result.languages = languageArg;
                         result.psm = psm;
                         result.elapsedMs = timer.elapsed();
                         if (status != QProcess::NormalExit || code != 0) {
                             result.error = QStringLiteral("tesseract 退出码 %1：%2")
                                                .arg(code)
                                                .arg(QString::fromLocal8Bit(process->readAllStandardError())
                                                         .trimmed());
                         } else {
                             result.text = QString::fromUtf8(process->readAllStandardOutput());
                             result.lines = parseLines(result.text);
                             result.ok = true;
                         }
                         callback(result);
                         process->deleteLater();
                     });

    process->start(tesseractBinary(), args);
    if (!process->waitForStarted(5000)) {
        OcrResult result;
        result.error = QStringLiteral("tesseract 无法启动");
        callback(result);
        process->deleteLater();
        return;
    }
    process->write(encodePng(image));
    process->closeWriteChannel();
}

OcrResult OcrEngine::recognize(const QImage &image, const QStringList &languages, int psm,
                               int timeoutMs)
{
    OcrResult result;
    result.psm = psm;
    if (image.isNull()) {
        result.error = QStringLiteral("空图像");
        return result;
    }
    QStringList effective = languages;
    if (effective.isEmpty()) {
        QString languageError;
        const QString resolved = defaultLanguages(&languageError);
        if (resolved.isEmpty()) {
            result.error = languageError;
            return result;
        }
        effective = resolved.split(QLatin1Char('+'));
    }
    result.languages = effective.join(QLatin1Char('+'));

    QProcess process;
    const QStringList args{QStringLiteral("-"), QStringLiteral("-"), QStringLiteral("-l"),
                           result.languages, QStringLiteral("--psm"), QString::number(psm)};
    QElapsedTimer timer;
    timer.start();
    process.start(tesseractBinary(), args);
    if (!process.waitForStarted(5000)) {
        result.error = QStringLiteral("tesseract 无法启动（装了没？）");
        return result;
    }
    process.write(encodePng(image));
    process.closeWriteChannel();
    if (!process.waitForFinished(timeoutMs)) {
        process.kill();
        result.error = QStringLiteral("tesseract 超时（%1ms）").arg(timeoutMs);
        return result;
    }
    result.elapsedMs = timer.elapsed();
    if (process.exitStatus() != QProcess::NormalExit || process.exitCode() != 0) {
        result.error = QStringLiteral("tesseract 退出码 %1：%2")
                           .arg(process.exitCode())
                           .arg(QString::fromLocal8Bit(process.readAllStandardError()).trimmed());
        return result;
    }
    result.text = QString::fromUtf8(process.readAllStandardOutput());
    result.lines = parseLines(result.text);
    result.ok = true;
    SS_INFO() << "[ocr] 识别完成" << result.summary();
    return result;
}

} // namespace seashot
