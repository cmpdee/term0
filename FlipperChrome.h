#pragma once

#include <QPoint>
#include <QTabBar>
#include <QWidget>

enum class ChromeTheme { Native, Green, Amber };

// Custom chrome is an opt-in preview used only by the Flipper display mode.
// Normal and green themes continue to use the platform window decorations.
class FlipperTitleBar final : public QWidget {
public:
    explicit FlipperTitleBar(QWidget *parent = nullptr);
    void setChromeTheme(ChromeTheme theme);
    void setDialogMode(bool enabled);

protected:
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void mouseDoubleClickEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    enum class Button { None, Minimize, Maximize, Close };
    QRect buttonRect(Button button) const;
    Button buttonAt(const QPoint &point) const;
    void triggerButton(Button button);

    ChromeTheme m_theme = ChromeTheme::Amber;
    bool m_dialogMode = false;
    Button m_hovered = Button::None;
    Button m_pressed = Button::None;
    bool m_manualDragging = false;
    QPoint m_dragOffset;
};

class FlipperTabBar final : public QTabBar {
public:
    explicit FlipperTabBar(QWidget *parent = nullptr);
    void setChromeTheme(ChromeTheme theme);
    bool customMode() const { return m_theme != ChromeTheme::Native; }

protected:
    QSize tabSizeHint(int index) const override;
    void paintEvent(QPaintEvent *event) override;
    void mousePressEvent(QMouseEvent *event) override;
    void mouseReleaseEvent(QMouseEvent *event) override;
    void mouseMoveEvent(QMouseEvent *event) override;
    void leaveEvent(QEvent *event) override;

private:
    QRect closeRect(int index) const;
    ChromeTheme m_theme = ChromeTheme::Native;
    int m_closePressed = -1;
    int m_closeHovered = -1;
};

class FlipperResizeHandle final : public QWidget {
public:
    FlipperResizeHandle(Qt::Edges edges, QWidget *parent);

protected:
    void mousePressEvent(QMouseEvent *event) override;

private:
    Qt::Edges m_edges;
};
