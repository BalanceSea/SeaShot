// SeaShot 的统一视觉规格（ISSUE-004）
//
// 设计原则：
//  - 一切颜色都来自系统调色板（QPalette），所以 Breeze 亮/暗主题自动跟随，不写死色值；
//  - 一切尺寸都来自下面的 spec 常量，改这里就能统一调整整套 UI；
//  - 图标统一从系统图标主题取（Breeze），带候选回退；取不到就退回纯文字，不崩不空。
//
// 规格表（对应用户可读的 docs/ui-spec.md）：
//   卡片圆角 8px / 按钮圆角 4px / 卡片内边距 12px / 元素间距 6px
//   底部操作条高 34px / 按钮高 24px / 按钮左右内边距 10px / 图标 16px
//   卡片描边：外圈 2px 深色(α100) + 内圈 2px 主题高亮 —— 保证浅色与深色背景都能一眼找到
//   交互态：hover = 底色提亮 8%，pressed = 底色压暗 15%，disabled = 文字降到 50% 透明度
#pragma once

#include <QColor>
#include <QIcon>
#include <QPalette>
#include <QPixmap>
#include <QString>
#include <QStringList>

namespace seashot::ui {

namespace spec {
inline constexpr int cardRadius = 8;
inline constexpr int buttonRadius = 4;
inline constexpr int cardPadding = 12;
inline constexpr int gap = 6;
inline constexpr int barHeight = 34;
inline constexpr int buttonHeight = 24;
inline constexpr int buttonPadX = 10;
inline constexpr int iconSize = 16;
inline constexpr int frameOuterWidth = 2;
inline constexpr int frameInnerWidth = 2;
inline constexpr int frameOuterAlpha = 100;
inline constexpr int frameInnerAlpha = 235;
inline constexpr int hoverLighten = 108;
inline constexpr int pressedDarker = 118;
inline constexpr int gapToPixels = 0; // 占位，保证 spec 里数值一眼可见
} // namespace spec

/// 卡片/按钮的统一配色（全部来自系统调色板）
struct CardColors {
    QColor card;          ///< 卡片底色（Window）
    QColor cardText;      ///< 卡片文字（WindowText）
    QColor bar;           ///< 操作条底色（Window + 少量透明）
    QColor button;        ///< 次要按钮底色（Button）
    QColor buttonText;    ///< 次要按钮文字（ButtonText）
    QColor primary;       ///< 主按钮底色（Highlight）
    QColor primaryText;   ///< 主按钮文字（HighlightedText）
    QColor border;        ///< 控件描边（Mid）
    QColor frameOuter;    ///< 卡片外圈描边
    QColor frameInner;    ///< 卡片内圈描边（主题高亮）
};

CardColors cardColors(const QPalette &palette);

/// 取系统主题图标：按 names 顺序回退；都没有则返回空 QIcon（调用方应退回纯文字）
QIcon icon(const QStringList &names);
QIcon icon(const QString &name);

// ── 语义图标（集中在这里，改主题只需改这一处）────────────────────────
QIcon iconEdit();        ///< 编辑
QIcon iconSaveAs();      ///< 另存为
QIcon iconCopy();        ///< 复制
QIcon iconClose();       ///< 关闭
QIcon iconRecord();      ///< 录制
QIcon iconStop();        ///< 停止
QIcon iconCapture();     ///< 截图
QIcon iconSettings();    ///< 设置
QIcon iconUndo();
QIcon iconRedo();
QIcon iconDelete();
QIcon iconSave();
QIcon iconRefresh();

/// 应用图标（内嵌在资源里；取不到时回退到主题的 camera-photo）
QPixmap appIconPixmap(int size);
QIcon appIcon();

/// 编辑器工具图标
QIcon iconToolSelect();
QIcon iconToolArrow();
QIcon iconToolRectangle();
QIcon iconToolEllipse();
QIcon iconToolPen();
QIcon iconToolText();

} // namespace seashot::ui
