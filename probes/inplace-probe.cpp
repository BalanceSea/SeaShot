// Phase 0 探针：就地编辑可行性（layer-shell 覆盖层上做标注 + 自绘工具条）
//
// 为什么先做这个：主链路（截图/录制/验收）现在全是绿的，而"就地编辑"要动到
// 截图交互的核心。本项目在 layer-shell 的输入区/合成上反复咬过人
// （ISSUE-001 拖动卡顿、ISSUE-007 自排除），所以先把地基单独验掉。
//
// 本探针**不碰主链路**，只回答四个问题：
//   1) LayerOverlay 层上能否稳定地 1:1 绘制一块"冻结屏幕"图？
//   2) 连续鼠标拖拽能否画出标注（帧率/延迟是否可接受）？
//   3) 同一表面内**自绘**的工具条能否正确接收点击？
//   4) 工具条以外的区域点击是否按预期处理（不误触发工具）？
//
// 用法：
//   probes/build/inplace-probe            # 用一张纯色测试图
//   probes/build/inplace-probe shot.png   # 用真实截图当"冻结屏幕"
//   SEASHOT_PROBE_HOLD=8000 ...           # 8 秒后自动退出（默认不自动退）
//
// 判读：
//   - 屏幕上应出现整幅图（1:1，不缩放）+ 半透明遮罩
//   - 按住左键拖动 → 出现蓝色虚线矩形（这就是"标注草稿"）
//   - 底部/顶部那条自绘工具条：点不同按钮会打印 TOOLBAR_CLICK <名字>
//   - 拖过工具条区域不应画出标注（说明输入区判定正确）
//   - 退出时打印 PROBE_SUMMARY，含拖拽帧数与平均帧间隔

#include <QApplication>
#include <QDateTime>
#include <QElapsedTimer>
#include <QImage>
#include <QKeyEvent>
#include <QMouseEvent>
#include <QPainter>
#include <QScreen>
#include <QTextStream>
#include <QTimer>
#include <QVector>
#include <QWidget>

#include <LayerShellQt/Window>

namespace {

constexpr int kToolbarHeight = 40;
constexpr int kToolbarButton = 32;
constexpr int kToolbarPad = 6;
constexpr int kToolbarGap = 4;

// 自绘工具条上的按钮（用文字代替图标，探针阶段只验"能不能正确命中"）
const QVector<QString> kTools{QStringLiteral("矩"), QStringLiteral("椭"), QStringLiteral("箭"),
                              QStringLiteral("笔"), QStringLiteral("文"), QStringLiteral("序"),
                              QStringLiteral("马"), QStringLiteral("退"), QStringLiteral("存"),
                              QStringLiteral("复")};

} // namespace

class InPlaceProbe : public QWidget
{
public:
    explicit InPlaceProbe(QWidget *parent = nullptr)
        : QWidget(parent)
    {
        setWindowFlags(Qt::FramelessWindowHint | Qt::Window);
        setAttribute(Qt::WA_TranslucentBackground, true);
        setMouseTracking(true);
        setWindowTitle(QStringLiteral("seashot-inplace-probe"));
    }

    void setFrozen(const QImage &image)
    {
        m_frozen = image;
        update();
    }

    /// 自绘工具条的矩形（贴选区下方；下方放不下则上方）
    QRect toolbarRect() const
    {
        const int width = kToolbarPad * 2 + kTools.size() * kToolbarButton
            + (kTools.size() - 1) * kToolbarGap;
        const int height = kToolbarHeight;
        int x = m_selection.left();
        int y = m_selection.bottom() + 6;
        if (y + height > m_surface.height() - 4)
            y = m_selection.top() - height - 6; // 上方
        if (y < 4)
            y = qMin(m_selection.bottom() + 6, m_surface.height() - height - 4);
        x = qBound(4, x, qMax(4, m_surface.width() - width - 4));
        return QRect(x, y, width, height);
    }

    QVector<QRect> toolButtonRects() const
    {
        QVector<QRect> rects;
        const QRect bar = toolbarRect();
        int x = bar.left() + kToolbarPad;
        for (int i = 0; i < kTools.size(); ++i) {
            rects.append(QRect(x, bar.top() + (kToolbarHeight - kToolbarButton) / 2, kToolbarButton,
                               kToolbarButton));
            x += kToolbarButton + kToolbarGap;
        }
        return rects;
    }

    int toolAt(const QPoint &position) const
    {
        const QVector<QRect> rects = toolButtonRects();
        for (int i = 0; i < rects.size(); ++i) {
            if (rects.at(i).contains(position))
                return i;
        }
        return -1;
    }

    void ensureLayerShell()
    {
        if (m_layerShellApplied)
            return;
        createWinId();
        if (QWindow *handle = windowHandle()) {
            if (auto *layerShell = LayerShellQt::Window::get(handle)) {
                layerShell->setLayer(LayerShellQt::Window::LayerOverlay);
                layerShell->setAnchors(LayerShellQt::Window::Anchors(
                    LayerShellQt::Window::AnchorTop | LayerShellQt::Window::AnchorBottom
                    | LayerShellQt::Window::AnchorLeft | LayerShellQt::Window::AnchorRight));
                layerShell->setMargins(QMargins(0, 0, 0, 0));
                layerShell->setExclusiveZone(-1);
                layerShell->setKeyboardInteractivity(
                    LayerShellQt::Window::KeyboardInteractivityOnDemand);
                m_layerShellApplied = true;
            }
        }
    }

    /// 表面几何 = 主屏（探针只跑单屏）
    QRect surfaceGeometry() const
    {
        QScreen *screen = QGuiApplication::primaryScreen();
        return screen ? screen->geometry() : QRect(0, 0, 1920, 1080);
    }

    void begin()
    {
        m_surface = surfaceGeometry();
        resize(m_surface.size());
        move(m_surface.topLeft());
        // ⚠️ 必须在 show() **之前**设定 shell：一旦表面按普通 toplevel 创建，
        //    再设 layer-shell 就会失败并报
        //    "Cannot set shell integration while there's already a shell surface created"
        //    主项目的 Toast 之所以正常，就是因为它经由 reposition() 在 show() 之前调用了这里。
        ensureLayerShell();
        show();
        raise();
        activateWindow();
        setFocus(Qt::OtherFocusReason);
        QTextStream out(stdout);
        out << "PROBE_BEGIN surface=" << m_surface.width() << "x" << m_surface.height()
            << " frozen=" << m_frozen.width() << "x" << m_frozen.height()
            << " layerShell=" << (m_layerShellApplied ? 1 : 0) << "\n";
        out.flush();
    }

protected:
    void paintEvent(QPaintEvent *) override
    {
        QPainter painter(this);
        painter.fillRect(rect(), QColor(0, 0, 0, 90)); // 半透明遮罩

        // 1) 冻结屏幕 1:1 绘制（不做任何缩放：就地编辑的核心约定）
        if (!m_frozen.isNull())
            painter.drawImage(0, 0, m_frozen);

        // 2) 选区 + 8 手柄
        if (m_selection.isValid()) {
            painter.setPen(QPen(QColor(0, 160, 255), 1));
            painter.setBrush(Qt::NoBrush);
            painter.drawRect(m_selection);
            painter.setBrush(QColor(0, 160, 255));
            for (const QPoint &p : handlePoints())
                painter.drawRect(QRect(p.x() - 3, p.y() - 3, 6, 6));
        }

        // 3) 标注草稿（拖动中）
        if (m_dragging && m_draft.isValid()) {
            painter.setPen(QPen(QColor(0, 160, 255), 2, Qt::DashLine));
            painter.setBrush(QColor(0, 160, 255, 40));
            painter.drawRect(m_draft);
        }

        // 4) 自绘工具条
        const QRect bar = toolbarRect();
        painter.setPen(Qt::NoPen);
        painter.setBrush(QColor(32, 34, 38, 235));
        painter.drawRoundedRect(bar, 8, 8);
        const QVector<QRect> rects = toolButtonRects();
        for (int i = 0; i < rects.size(); ++i) {
            const bool hovered = i == m_hoverTool;
            painter.setBrush(hovered ? QColor(0, 120, 215) : QColor(60, 63, 70));
            painter.drawRoundedRect(rects.at(i), 6, 6);
            painter.setPen(QColor(235, 238, 245));
            painter.drawText(rects.at(i), Qt::AlignCenter, kTools.at(i));
            painter.setPen(Qt::NoPen);
        }
    }

    void mousePressEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton)
            return;
        const QPoint pos = event->position().toPoint();
        // 关键判定：工具条优先，且**先于**标注逻辑
        const int tool = toolAt(pos);
        if (tool >= 0) {
            m_pressedTool = tool;
            ++m_toolbarHits;
            QTextStream(stdout) << "TOOLBAR_CLICK " << kTools.at(tool) << " index=" << tool << "\n";
            QTextStream(stdout).flush();
            update();
            return;
        }
        // 点在工具条矩形内但不是按钮 → 算"落在工具条上"，不应开始标注
        if (toolbarRect().contains(pos)) {
            ++m_toolbarHits;
            QTextStream(stdout) << "TOOLBAR_CLICK (空隙)\n";
            QTextStream(stdout).flush();
            return;
        }
        m_dragging = true;
        m_dragStart = pos;
        m_draft = QRect(pos, pos);
        m_elapsed.start();
        m_dragFrames = 0;
        m_lastMoveMs = -1;
        update();
    }

    void mouseMoveEvent(QMouseEvent *event) override
    {
        const QPoint pos = event->position().toPoint();
        if (!m_dragging) {
            const int hovered = toolAt(pos);
            if (hovered != m_hoverTool) {
                m_hoverTool = hovered;
                update();
            }
            return;
        }
        ++m_dragFrames;
        m_draft = QRect(m_dragStart, pos).normalized();
        if (m_lastMoveMs >= 0)
            m_intervals.append(m_elapsed.elapsed() - m_lastMoveMs);
        m_lastMoveMs = m_elapsed.elapsed();
        update();
    }

    void mouseReleaseEvent(QMouseEvent *event) override
    {
        if (event->button() != Qt::LeftButton)
            return;
        if (m_pressedTool >= 0) {
            m_pressedTool = -1;
            update();
            return;
        }
        if (m_dragging) {
            m_dragging = false;
            m_selection = m_draft;
            QTextStream(stdout) << "ANNOTATION_DONE rect=" << m_selection.x() << ","
                                << m_selection.y() << " " << m_selection.width() << "x"
                                << m_selection.height() << "\n";
            QTextStream(stdout).flush();
            update();
        }
    }

    void keyPressEvent(QKeyEvent *event) override
    {
        if (event->key() == Qt::Key_Escape)
            summarize();
        QWidget::keyPressEvent(event);
    }

public:
    void summarize()
    {
        QTextStream out(stdout);
        double avg = 0.0;
        int maxInterval = 0;
        for (int v : m_intervals) {
            avg += v;
            maxInterval = qMax(maxInterval, v);
        }
        if (!m_intervals.isEmpty())
            avg /= double(m_intervals.size());
        out << "PROBE_SUMMARY dragFrames=" << m_dragFrames << " avgMoveGapMs=" << avg
            << " maxMoveGapMs=" << maxInterval << " toolbarHits=" << m_toolbarHits
            << " layerShell=" << (m_layerShellApplied ? 1 : 0) << "\n";
        out.flush();
        QCoreApplication::quit();
    }

private:
    QVector<QPoint> handlePoints() const
    {
        if (!m_selection.isValid())
            return {};
        const QRect r = m_selection;
        return {r.topLeft(), QPoint(r.center().x(), r.top()), r.topRight(),
                QPoint(r.left(), r.center().y()), QPoint(r.right(), r.center().y()),
                r.bottomLeft(), QPoint(r.center().x(), r.bottom()), r.bottomRight()};
    }

    QImage m_frozen;
    QRect m_surface;
    QRect m_selection;
    QRect m_draft;
    bool m_dragging = false;
    QPoint m_dragStart;
    bool m_layerShellApplied = false;
    int m_hoverTool = -1;
    int m_pressedTool = -1;
    int m_toolbarHits = 0;
    int m_dragFrames = 0;
    qint64 m_lastMoveMs = -1;
    QVector<int> m_intervals;
    QElapsedTimer m_elapsed;
};

int main(int argc, char **argv)
{
    QApplication app(argc, argv);
    InPlaceProbe probe;

    QImage frozen;
    if (argc > 1)
        frozen = QImage(QString::fromLocal8Bit(argv[1]));
    if (frozen.isNull()) {
        // 没有真实截图时造一张有内容与细纹的测试图：
        // 1:1 是否失真、拖拽时有没有撕裂，都能看出来
        const QRect surface = probe.surfaceGeometry();
        frozen = QImage(surface.size(), QImage::Format_RGB32);
        QPainter painter(&frozen);
        painter.fillRect(frozen.rect(), QColor(24, 26, 30));
        painter.setPen(QColor(220, 225, 235));
        for (int y = 0; y < frozen.height(); y += 40)
            painter.drawLine(0, y, frozen.width(), y);
        for (int x = 0; x < frozen.width(); x += 40)
            painter.drawLine(x, 0, x, frozen.height());
        painter.drawText(QRect(0, 0, frozen.width(), frozen.height()), Qt::AlignCenter,
                         QStringLiteral("inplace-probe 测试图（1:1，不应缩放）"));
    }
    probe.setFrozen(frozen);
    probe.begin();

    const int hold = qEnvironmentVariableIntValue("SEASHOT_PROBE_HOLD");
    if (hold > 0)
        QTimer::singleShot(hold, &probe, &InPlaceProbe::summarize);
    return app.exec();
}
