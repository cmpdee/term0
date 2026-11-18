#include "ChromeDialog.h"

#include <QEvent>
#include <QGuiApplication>
#include <QLayout>
#include <QResizeEvent>

ChromeDialog::ChromeDialog(QWidget *parent) : QDialog(parent) {
    m_titleBar = new FlipperTitleBar(this);
    m_titleBar->setDialogMode(true);
    m_titleBar->hide();

    for (Qt::Edges edges : {Qt::Edges(Qt::LeftEdge), Qt::Edges(Qt::RightEdge),
                            Qt::Edges(Qt::TopEdge), Qt::Edges(Qt::BottomEdge),
                            Qt::Edges(Qt::TopEdge | Qt::LeftEdge),
                            Qt::Edges(Qt::TopEdge | Qt::RightEdge),
                            Qt::Edges(Qt::BottomEdge | Qt::LeftEdge),
                            Qt::Edges(Qt::BottomEdge | Qt::RightEdge)}) {
        m_handles.append(new FlipperResizeHandle(edges, this));
    }
}

void ChromeDialog::setChromeTheme(ChromeTheme theme) {
    if (m_theme == theme)
        return;

    const bool wasVisible = isVisible();
    QRect targetRect = geometry();
    const bool enabled = theme != ChromeTheme::Native;
    const bool wasCustom = m_theme != ChromeTheme::Native;
    // Keep the usable content area roughly the same size on a native/custom
    // transition; the custom title bar consumes 40 client-area pixels.
    if (enabled != wasCustom)
        targetRect.setHeight(qMax(minimumHeight(), targetRect.height() + (enabled ? 40 : -40)));
    m_theme = theme;
    m_titleBar->setChromeTheme(theme);

    // The content layout is installed by the dialog's owner. Reserve vertical
    // space instead of overlaying the painted bar on top of fields/buttons.
    if (auto *root = layout()) {
        if (!m_haveOriginalMargins) {
            m_originalMargins = root->contentsMargins();
            m_haveOriginalMargins = true;
        }
        QMargins margins = m_originalMargins;
        if (enabled)
            margins.setTop(margins.top() + 40);
        root->setContentsMargins(margins);
    }

    setWindowFlag(Qt::FramelessWindowHint, enabled);
    if (wasVisible) {
        show();
        setGeometry(targetRect);
    } else {
        resize(targetRect.size());
    }
    m_titleBar->setVisible(enabled);
    placeChrome();
}

void ChromeDialog::placeChrome() {
    m_titleBar->setGeometry(0, 0, width(), 40);
    if (m_theme != ChromeTheme::Native)
        m_titleBar->raise();
    placeHandles();
}

void ChromeDialog::placeHandles() {
    const bool enabled = m_theme != ChromeTheme::Native && !isMaximized();
    if (m_handles.size() != 8)
        return;
    constexpr int edge = 6;
    constexpr int corner = 11;
    const int w = width();
    const int h = height();
    const QRect hitAreas[] = {
        {0, corner, edge, qMax(0, h - 2 * corner)},
        {w - edge, corner, edge, qMax(0, h - 2 * corner)},
        {corner, 0, qMax(0, w - 2 * corner), edge},
        {corner, h - edge, qMax(0, w - 2 * corner), edge},
        {0, 0, corner, corner}, {w - corner, 0, corner, corner},
        {0, h - corner, corner, corner}, {w - corner, h - corner, corner, corner}
    };
    for (int i = 0; i < m_handles.size(); ++i) {
        auto *handle = m_handles[i];
        handle->setGeometry(hitAreas[i]);
        handle->setVisible(enabled);
        if (enabled)
            handle->raise();
    }
}

void ChromeDialog::resizeEvent(QResizeEvent *event) {
    QDialog::resizeEvent(event);
    placeChrome();
}

void ChromeDialog::changeEvent(QEvent *event) {
    QDialog::changeEvent(event);
    if (event->type() == QEvent::WindowTitleChange ||
        event->type() == QEvent::WindowStateChange)
        m_titleBar->update();
    if (event->type() == QEvent::WindowStateChange)
        placeHandles();
}
