#include "FlipperChrome.h"

#include <QApplication>
#include <QCursor>
#include <QGuiApplication>
#include <QSizePolicy>
#include <QColor>
#include <QFontDatabase>
#include <QMouseEvent>
#include <QPainter>
#include <QPaintEvent>
#include <QWindow>

namespace {
const QColor kAmber(255, 157, 0);
const QColor kAmberBright(255, 187, 56);
const QColor kAmberDim(167, 94, 6);
const QColor kBackground(5, 4, 3);
const QColor kPanel(2, 2, 1);
struct ChromeColors {
    QColor accent;
    QColor bright;
    QColor dim;
    QColor background;
    QColor panel;
    QColor hover;
};
ChromeColors themeColors(ChromeTheme theme) {
    if (theme == ChromeTheme::Green)
        return {QColor(124, 255, 143), QColor(189, 255, 194),
                QColor(49, 83, 58), QColor(7, 10, 8),
                QColor(2, 4, 3), QColor(22, 52, 30)};
    return {kAmber, kAmberBright, kAmberDim,
            kBackground, kPanel, QColor(44, 27, 4)};
}

QFont chromeFont(QWidget *widget) {
    QFont font = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    if (widget->font().pointSizeF() > 0)
        font.setPointSizeF(widget->font().pointSizeF());
    font.setBold(true);
    return font;
}
}

void FlipperTitleBar::setChromeTheme(ChromeTheme theme) {
    if (m_theme == theme)
        return;
    m_theme = theme;
    update();
}

void FlipperTitleBar::setDialogMode(bool enabled) {
    m_dialogMode = enabled;
    update();
}

FlipperTitleBar::FlipperTitleBar(QWidget *parent) : QWidget(parent) {
    setFixedHeight(40);
    setMouseTracking(true);
    setFocusPolicy(Qt::NoFocus);
    setAccessibleName(QStringLiteral("term0 custom title bar"));
    setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
}

QRect FlipperTitleBar::buttonRect(Button button) const {
    constexpr int buttonWidth = 44;
    const int fromRight = button == Button::Close ? 1
                        : button == Button::Maximize ? 2 : 3;
    return QRect(width() - buttonWidth * fromRight, 0,
                 buttonWidth, height() - 1);
}

FlipperTitleBar::Button FlipperTitleBar::buttonAt(const QPoint &point) const {
    for (Button button : {Button::Close, Button::Maximize, Button::Minimize}) {
        if (m_dialogMode && button != Button::Close)
            continue;
        if (buttonRect(button).contains(point))
            return button;
    }
    return Button::None;
}

void FlipperTitleBar::paintEvent(QPaintEvent *) {
    const auto colors = themeColors(m_theme);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);
    p.fillRect(rect(), colors.panel);
    p.fillRect(QRect(0, height() - 2, width(), 2), colors.accent);
    p.setFont(chromeFont(this));
    p.setPen(colors.accent);

    // term0's own prompt, without device branding or external artwork.
    p.drawText(QRect(14, 0, 35, height() - 2),
               Qt::AlignVCenter | Qt::AlignLeft, QStringLiteral(">0"));
    const QString caption = window()->windowTitle();
    p.drawText(QRect(56, 0, qMax(0, width() - (m_dialogMode ? 114 : 200)), height() - 2),
               Qt::AlignVCenter | Qt::AlignLeft,
               p.fontMetrics().elidedText(caption, Qt::ElideRight,
                  qMax(0, width() - (m_dialogMode ? 114 : 200))));

    for (Button button : {Button::Minimize, Button::Maximize, Button::Close}) {
        if (m_dialogMode && button != Button::Close)
            continue;
        const QRect area = buttonRect(button);
        if (button == m_pressed)
            p.fillRect(area, colors.accent);
        else if (button == m_hovered)
            p.fillRect(area, colors.hover);
        const QColor ink = button == m_pressed ? colors.panel : colors.accent;
        QPen pen(ink, 2);
        pen.setCapStyle(Qt::SquareCap);
        p.setPen(pen);
        const QPoint c = area.center();
        if (button == Button::Minimize) {
            p.drawLine(c.x() - 7, c.y() + 5, c.x() + 7, c.y() + 5);
        } else if (button == Button::Maximize) {
            if (window()->isMaximized()) {
                p.drawRect(QRect(c.x() - 5, c.y() - 4, 11, 10));
                p.drawLine(c.x() - 3, c.y() - 6, c.x() + 7, c.y() - 6);
                p.drawLine(c.x() + 7, c.y() - 6, c.x() + 7, c.y() + 3);
            } else {
                p.drawRect(QRect(c.x() - 7, c.y() - 6, 14, 12));
            }
        } else {
            p.drawLine(c.x() - 6, c.y() - 6, c.x() + 6, c.y() + 6);
            p.drawLine(c.x() + 6, c.y() - 6, c.x() - 6, c.y() + 6);
        }
    }
}

void FlipperTitleBar::mousePressEvent(QMouseEvent *event) {
    if (event->button() != Qt::LeftButton) {
        QWidget::mousePressEvent(event);
        return;
    }
    m_pressed = buttonAt(event->position().toPoint());
    update();
    if (m_pressed != Button::None) {
        event->accept();
        return;
    }
    // Let the compositor handle move/snap; works on Windows, X11 and Wayland
    // where supported (manual move() is not valid on Wayland).
    bool systemMoveStarted = false;
    if (auto *handle = window()->windowHandle())
        systemMoveStarted = handle->startSystemMove();
    if (!systemMoveStarted && !window()->isMaximized() &&
        !QGuiApplication::platformName().contains(QStringLiteral("wayland"),
                                                   Qt::CaseInsensitive)) {
        // Last-resort fallback for X11/Windows window managers that reject
        // startSystemMove(). Wayland explicitly forbids programmatic moves.
        m_manualDragging = true;
        m_dragOffset = event->globalPosition().toPoint() - window()->pos();
    }
    event->accept();
}

void FlipperTitleBar::triggerButton(Button button) {
    switch (button) {
    case Button::Minimize: window()->showMinimized(); break;
    case Button::Maximize:
        if (window()->isMaximized()) window()->showNormal();
        else window()->showMaximized();
        break;
    case Button::Close: window()->close(); break;
    case Button::None: break;
    }
}

void FlipperTitleBar::mouseReleaseEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) {
        const Button released = buttonAt(event->position().toPoint());
        const Button pressed = m_pressed;
        m_pressed = Button::None;
        m_manualDragging = false;
        update();
        if (pressed != Button::None && pressed == released)
            triggerButton(pressed);
        event->accept();
        return;
    }
    QWidget::mouseReleaseEvent(event);
}

void FlipperTitleBar::mouseMoveEvent(QMouseEvent *event) {
    if (m_manualDragging && (event->buttons() & Qt::LeftButton)) {
        window()->move(event->globalPosition().toPoint() - m_dragOffset);
        event->accept();
        return;
    }
    const Button hovered = buttonAt(event->position().toPoint());
    if (m_hovered != hovered) {
        m_hovered = hovered;
        update();
    }
    QWidget::mouseMoveEvent(event);
}

void FlipperTitleBar::mouseDoubleClickEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton &&
        buttonAt(event->position().toPoint()) == Button::None) {
        triggerButton(Button::Maximize);
        event->accept();
        return;
    }
    QWidget::mouseDoubleClickEvent(event);
}

void FlipperTitleBar::leaveEvent(QEvent *event) {
    m_hovered = Button::None;
    m_pressed = Button::None;
    m_manualDragging = false;
    update();
    QWidget::leaveEvent(event);
}

FlipperTabBar::FlipperTabBar(QWidget *parent) : QTabBar(parent) {
    setMouseTracking(true);
    setFocusPolicy(Qt::NoFocus);
}

void FlipperTabBar::setChromeTheme(ChromeTheme theme) {
    if (m_theme == theme)
        return;
    m_theme = theme;
    m_closePressed = -1;
    m_closeHovered = -1;
    updateGeometry();
    update();
}

QSize FlipperTabBar::tabSizeHint(int index) const {
    if (m_theme == ChromeTheme::Native)
        return QTabBar::tabSizeHint(index);
    const int textWidth = fontMetrics().horizontalAdvance(tabText(index));
    return QSize(qBound(120, textWidth + 58, 260), 36);
}

QRect FlipperTabBar::closeRect(int index) const {
    const QRect area = tabRect(index);
    return QRect(area.right() - 29, area.y() + (area.height() - 24) / 2,
                 24, 24);
}

void FlipperTabBar::paintEvent(QPaintEvent *event) {
    if (m_theme == ChromeTheme::Native) {
        QTabBar::paintEvent(event);
        return;
    }
    const auto colors = themeColors(m_theme);
    QPainter p(this);
    p.setRenderHint(QPainter::Antialiasing, false);
    p.fillRect(rect(), colors.background);
    p.setFont(chromeFont(this));
    for (int i = 0; i < count(); ++i) {
        const QRect area = tabRect(i);
        if (!area.intersects(event->rect()))
            continue;
        const bool selected = i == currentIndex();
        const bool hovered = i == tabAt(mapFromGlobal(QCursor::pos()));
        p.fillRect(area.adjusted(0, 0, -1, -1),
                   selected ? colors.accent : (hovered ? colors.hover : colors.panel));
        p.setPen(QPen(selected ? colors.bright : colors.dim, 1));
        p.drawRect(area.adjusted(0, 0, -1, -1));
        p.setPen(selected ? colors.panel : colors.accent);
        const QRect label = area.adjusted(12, 0, -33, 0);
        p.drawText(label, Qt::AlignVCenter | Qt::AlignLeft,
                   p.fontMetrics().elidedText(tabText(i), Qt::ElideRight, label.width()));
        if (!tabsClosable() && count() > 1) {
            const QRect close = closeRect(i);
            if (m_closeHovered == i)
                p.fillRect(close, selected ? colors.bright : colors.hover);
            QPen pen(selected ? colors.panel : colors.accent, 2);
            pen.setCapStyle(Qt::SquareCap);
            p.setPen(pen);
            const QPoint c = close.center();
            p.drawLine(c.x() - 4, c.y() - 4, c.x() + 4, c.y() + 4);
            p.drawLine(c.x() + 4, c.y() - 4, c.x() - 4, c.y() + 4);
        }
    }
}

void FlipperTabBar::mousePressEvent(QMouseEvent *event) {
    if (m_theme != ChromeTheme::Native && event->button() == Qt::LeftButton) {
        const int index = tabAt(event->position().toPoint());
        if (index >= 0 && count() > 1 && closeRect(index).contains(event->position().toPoint())) {
            m_closePressed = index;
            event->accept();
            return;
        }
    }
    QTabBar::mousePressEvent(event);
}

void FlipperTabBar::mouseReleaseEvent(QMouseEvent *event) {
    if (m_theme != ChromeTheme::Native && m_closePressed >= 0 && event->button() == Qt::LeftButton) {
        const int index = m_closePressed;
        m_closePressed = -1;
        if (index < count() && closeRect(index).contains(event->position().toPoint()))
            emit tabCloseRequested(index);
        event->accept();
        return;
    }
    QTabBar::mouseReleaseEvent(event);
}

void FlipperTabBar::mouseMoveEvent(QMouseEvent *event) {
    if (m_theme != ChromeTheme::Native) {
        const int index = tabAt(event->position().toPoint());
        const int hovered = index >= 0 && count() > 1 &&
                            closeRect(index).contains(event->position().toPoint()) ? index : -1;
        if (hovered != m_closeHovered) {
            m_closeHovered = hovered;
            update();
        }
    }
    QTabBar::mouseMoveEvent(event);
}

void FlipperTabBar::leaveEvent(QEvent *event) {
    m_closeHovered = -1;
    m_closePressed = -1;
    update();
    QTabBar::leaveEvent(event);
}

FlipperResizeHandle::FlipperResizeHandle(Qt::Edges edges, QWidget *parent)
    : QWidget(parent), m_edges(edges) {
    setFocusPolicy(Qt::NoFocus);
    setAttribute(Qt::WA_NoSystemBackground);
    setAttribute(Qt::WA_TranslucentBackground);
    if (edges == Qt::LeftEdge || edges == Qt::RightEdge)
        setCursor(Qt::SizeHorCursor);
    else if (edges == Qt::TopEdge || edges == Qt::BottomEdge)
        setCursor(Qt::SizeVerCursor);
    else if (edges == (Qt::TopEdge | Qt::LeftEdge) ||
             edges == (Qt::BottomEdge | Qt::RightEdge))
        setCursor(Qt::SizeFDiagCursor);
    else
        setCursor(Qt::SizeBDiagCursor);
    hide();
}

void FlipperResizeHandle::mousePressEvent(QMouseEvent *event) {
    if (event->button() == Qt::LeftButton) {
        if (auto *handle = window()->windowHandle())
            handle->startSystemResize(m_edges);
        event->accept();
        return;
    }
    QWidget::mousePressEvent(event);
}
