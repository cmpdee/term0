#include "MainWindow.h"
#include "SessionWidget.h"
#include "FlipperChrome.h"

#include <QApplication>
#include <QCloseEvent>
#include <QEvent>
#include <QResizeEvent>
#include <QCoreApplication>
#include <QDir>
#include <QFontDatabase>
#include <QKeySequence>
#include <QPalette>
#include <QSettings>
#include <QShortcut>
#include <QStackedWidget>
#include <QStandardPaths>
#include <QTabBar>
#include <QTimer>
#include <QVBoxLayout>
#include <QWidget>

#ifdef Q_OS_WIN
#include <windows.h>
#include <dwmapi.h>
#endif

namespace {
QString configDirPath() {
#ifdef Q_OS_WIN
    QString base = qEnvironmentVariable("APPDATA");
    if (base.isEmpty())
        base = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
#elif defined(Q_OS_LINUX)
    QString base = QStandardPaths::writableLocation(QStandardPaths::GenericConfigLocation);
#else
    QString base = QStandardPaths::writableLocation(QStandardPaths::AppConfigLocation);
#endif

    QDir dir(base);
    dir.mkpath(QStringLiteral("term0"));
    return dir.filePath(QStringLiteral("term0"));
}

QString settingsFilePath() {
    return QDir(configDirPath()).filePath(QStringLiteral("term0.ini"));
}

#ifdef Q_OS_WIN
void applyWindowsDarkTitleBarOnce(QWidget *widget, bool enabled) {
    if (!widget)
        return;

    HWND hwnd = reinterpret_cast<HWND>(widget->winId());
    if (!hwnd)
        return;

    const BOOL value = enabled ? TRUE : FALSE;
    constexpr DWORD kImmersiveDarkMode = 20;
    constexpr DWORD kImmersiveDarkModeLegacy = 19;
    HRESULT hr = DwmSetWindowAttribute(hwnd, kImmersiveDarkMode,
                                       &value, sizeof(value));
    if (FAILED(hr)) {
        DwmSetWindowAttribute(hwnd, kImmersiveDarkModeLegacy,
                              &value, sizeof(value));
    }

    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                 SWP_NOACTIVATE | SWP_FRAMECHANGED);
    RedrawWindow(hwnd, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_FRAME | RDW_UPDATENOW);
    DwmFlush();
}

void setWindowsDarkTitleBar(QWidget *widget, bool enabled) {
    applyWindowsDarkTitleBarOnce(widget, enabled);
    QTimer::singleShot(0, widget, [widget, enabled] {
        applyWindowsDarkTitleBarOnce(widget, enabled);
    });
}

bool systemUsesDarkWindowChrome() {
    return QApplication::palette().color(QPalette::Window).lightness() < 128;
}
#else
void setWindowsDarkTitleBar(QWidget *, bool) {}
bool systemUsesDarkWindowChrome() { return false; }
#endif
}

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    m_standardUiFont = QApplication::font();
    buildUi();
    loadWindowPreferences();
    addSession();
}

MainWindow::~MainWindow() {
    saveWindowPreferences();
}

void MainWindow::buildUi() {
    setWindowTitle(QStringLiteral("term0 %1")
                       .arg(QCoreApplication::applicationVersion()));
    resize(900, 600);

    auto *host = new QWidget(this);
    host->setObjectName(QStringLiteral("tabHost"));
    auto *layout = new QVBoxLayout(host);
    layout->setContentsMargins(0, 0, 0, 0);
    layout->setSpacing(0);

    m_titleBar = new FlipperTitleBar(host);
    m_titleBar->hide();
    layout->addWidget(m_titleBar);

    m_tabBar = new FlipperTabBar(host);
    m_tabBar->setDocumentMode(true);
    m_tabBar->setExpanding(false);
    m_tabBar->setMovable(false);
    m_tabBar->setTabsClosable(false);
    m_tabBar->setFocusPolicy(Qt::NoFocus);
    m_tabBar->hide();

    m_stack = new QStackedWidget(host);

    layout->addWidget(m_tabBar);
    layout->addWidget(m_stack, 1);
    setCentralWidget(host);

    // Invisible 6 px edge / 11 px corner hit targets provide compositor-owned
    // resizing in frameless mode, including high-DPI displays and Wayland.
    for (Qt::Edges edges : {Qt::Edges(Qt::LeftEdge), Qt::Edges(Qt::RightEdge),
                            Qt::Edges(Qt::TopEdge), Qt::Edges(Qt::BottomEdge),
                            Qt::Edges(Qt::TopEdge | Qt::LeftEdge),
                            Qt::Edges(Qt::TopEdge | Qt::RightEdge),
                            Qt::Edges(Qt::BottomEdge | Qt::LeftEdge),
                            Qt::Edges(Qt::BottomEdge | Qt::RightEdge)}) {
        m_resizeHandles.append(new FlipperResizeHandle(edges, this));
    }

    connect(m_tabBar, &QTabBar::currentChanged,
            this, &MainWindow::activateSession);
    connect(m_tabBar, &QTabBar::tabCloseRequested,
            this, &MainWindow::closeSession);

    auto *newTabShortcut = new QShortcut(
        QKeySequence(QStringLiteral("Ctrl+Shift+T")), this);
    connect(newTabShortcut, &QShortcut::activated,
            this, &MainWindow::addSession);

    auto *closeTabShortcut = new QShortcut(
        QKeySequence(QStringLiteral("Ctrl+W")), this);
    connect(closeTabShortcut, &QShortcut::activated,
            this, &MainWindow::closeCurrentSession);

    auto *nextTabShortcut = new QShortcut(
        QKeySequence(QStringLiteral("Ctrl+Tab")), this);
    connect(nextTabShortcut, &QShortcut::activated, this, [this] {
        const int count = m_tabBar->count();
        if (count > 1)
            m_tabBar->setCurrentIndex((m_tabBar->currentIndex() + 1) % count);
    });

    auto *previousTabShortcut = new QShortcut(
        QKeySequence(QStringLiteral("Ctrl+Shift+Tab")), this);
    connect(previousTabShortcut, &QShortcut::activated, this, [this] {
        const int count = m_tabBar->count();
        if (count > 1)
            m_tabBar->setCurrentIndex((m_tabBar->currentIndex() + count - 1) % count);
    });
}

void MainWindow::addSession() {
    const int sessionNumber = nextAvailableSessionNumber();
    auto *session = new SessionWidget(m_standardUiFont, m_stack);
    session->setProperty("sessionNumber", sessionNumber);
    const int stackIndex = m_stack->addWidget(session);
    const int tabIndex = m_tabBar->addTab(
        QStringLiteral("Session %1").arg(sessionNumber));

    Q_ASSERT(stackIndex == tabIndex);

    session->setSessionLabel(QStringLiteral("Session %1").arg(sessionNumber));
    connect(session, &SessionWidget::tabTitleChanged,
            this, [this, session](const QString &title) {
        const int index = m_stack->indexOf(session);
        if (index >= 0 && !title.trimmed().isEmpty()) {
            m_tabBar->setTabText(index, title.trimmed());
            session->setSessionLabel(title.trimmed());
        }
    });
    connect(session, &SessionWidget::hiddenThemeToggleRequested,
            this, &MainWindow::toggleHiddenTheme);
    connect(session, &SessionWidget::flipperThemeToggleRequested,
            this, &MainWindow::toggleFlipperTheme);

    if (m_hiddenTheme)
        session->setHiddenTheme(true);
    else if (m_flipperTheme)
        session->setFlipperTheme(true);
    m_tabBar->setCurrentIndex(tabIndex);
    m_stack->setCurrentIndex(stackIndex);
    updateTabBarVisibility();
    session->focusInput();
}

void MainWindow::closeCurrentSession() {
    if (m_tabBar->count() <= 1)
        return;
    closeSession(m_tabBar->currentIndex());
}

void MainWindow::closeSession(int index) {
    if (m_tabBar->count() <= 1 || index < 0 || index >= m_tabBar->count())
        return;

    QWidget *page = m_stack->widget(index);
    if (auto *session = qobject_cast<SessionWidget *>(page))
        session->setSessionActive(false);
    m_stack->removeWidget(page);
    m_tabBar->removeTab(index);
    if (page)
        page->deleteLater();

    const int next = qMin(index, m_tabBar->count() - 1);
    m_tabBar->setCurrentIndex(next);
    m_stack->setCurrentIndex(next);
    updateTabBarVisibility();

    activateSession(next);
}

void MainWindow::activateSession(int index) {
    if (index < 0 || index >= m_stack->count())
        return;
    auto *next = qobject_cast<SessionWidget *>(m_stack->widget(index));
    if (m_activeSession && m_activeSession != next)
        m_activeSession->setSessionActive(false);
    m_stack->setCurrentIndex(index);
    m_activeSession = next;
    if (next) {
        next->setSessionActive(true);
        next->focusInput();
    }
}

void MainWindow::updateTabBarVisibility() {
    const bool multiple = m_tabBar->count() > 1;
    m_tabBar->setTabsClosable(multiple && !m_flipperTheme && !m_hiddenTheme);
    m_tabBar->setVisible(multiple);
}

SessionWidget *MainWindow::currentSession() const {
    return qobject_cast<SessionWidget *>(m_stack->currentWidget());
}

int MainWindow::nextAvailableSessionNumber() const {
    int candidate = 1;
    for (;;) {
        bool used = false;
        for (int i = 0; i < m_stack->count(); ++i) {
            const QWidget *page = m_stack->widget(i);
            if (page && page->property("sessionNumber").toInt() == candidate) {
                used = true;
                break;
            }
        }
        if (!used)
            return candidate;
        ++candidate;
    }
}

void MainWindow::toggleHiddenTheme() {
    if (m_hiddenTheme) {
        applyHiddenTheme(false);
        return;
    }
    // Switch the painted chrome directly to green: do not briefly restore
    // native window flags between the two custom themes.
    m_flipperTheme = false;
    applyHiddenTheme(true);
}

void MainWindow::toggleFlipperTheme() {
    if (m_flipperTheme) {
        applyFlipperTheme(false);
        return;
    }
    m_hiddenTheme = false;
    applyFlipperTheme(true);
}

void MainWindow::applyHiddenTheme(bool enabled) {
    m_hiddenTheme = enabled;
    if (enabled)
        m_flipperTheme = false;
    m_titleBar->setChromeTheme(enabled ? ChromeTheme::Green : ChromeTheme::Native);
    setCustomChrome(enabled);
    m_tabBar->setChromeTheme(enabled ? ChromeTheme::Green : ChromeTheme::Native);
    updateTabBarVisibility();
    setWindowsDarkTitleBar(this,
        enabled ? true : systemUsesDarkWindowChrome());

    if (!enabled) {
        QApplication::setFont(m_standardUiFont);
        setFont(m_standardUiFont);
        setStyleSheet(QString());
    } else {
        QFont uiFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        if (m_standardUiFont.pointSizeF() > 0)
            uiFont.setPointSizeF(m_standardUiFont.pointSizeF());
        QApplication::setFont(uiFont);
        setFont(uiFont);
        setStyleSheet(QStringLiteral(R"(
            QWidget#tabHost {
                background: #070a08;
                color: #7cff8f;
            }
            QTabBar::tab {
                background: #020403;
                color: #7cff8f;
                border: 1px solid #31533a;
                border-radius: 0;
                padding: 5px 12px;
                margin-right: 1px;
            }
            QTabBar::tab:selected {
                background: #17351f;
                border-color: #7cff8f;
            }
            QTabBar::close-button {
                subcontrol-position: right;
            }
        )"));
    }

    for (int i = 0; i < m_stack->count(); ++i) {
        if (auto *session = qobject_cast<SessionWidget *>(m_stack->widget(i)))
            session->setHiddenTheme(enabled);
    }
}

void MainWindow::applyFlipperTheme(bool enabled) {
    m_flipperTheme = enabled;
    if (enabled)
        m_hiddenTheme = false;
    m_titleBar->setChromeTheme(enabled ? ChromeTheme::Amber : ChromeTheme::Native);
    setCustomChrome(enabled);
    m_tabBar->setChromeTheme(enabled ? ChromeTheme::Amber : ChromeTheme::Native);
    updateTabBarVisibility();
    setWindowsDarkTitleBar(this,
        enabled ? true : systemUsesDarkWindowChrome());

    if (!enabled) {
        QApplication::setFont(m_standardUiFont);
        setFont(m_standardUiFont);
        setStyleSheet(QString());
    } else {
        QFont uiFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        if (m_standardUiFont.pointSizeF() > 0)
            uiFont.setPointSizeF(m_standardUiFont.pointSizeF());
        QApplication::setFont(uiFont);
        setFont(uiFont);
        setStyleSheet(QStringLiteral(R"(
            QWidget#tabHost {
                background: #050403;
                color: #ff9d00;
            }
            QTabBar::tab {
                background: #050403;
                color: #ff9d00;
                border: 1px solid #b86f00;
                border-radius: 0;
                padding: 5px 12px;
                margin-right: 1px;
            }
            QTabBar::tab:selected {
                background: #ff9d00;
                color: #080500;
                border-color: #ffb12b;
            }
            QTabBar::tab:hover:!selected {
                background: #1a1003;
                border-color: #ff9d00;
            }
            QTabBar::close-button {
                subcontrol-position: right;
            }
        )"));
    }

    for (int i = 0; i < m_stack->count(); ++i) {
        if (auto *session = qobject_cast<SessionWidget *>(m_stack->widget(i)))
            session->setFlipperTheme(enabled);
    }
}


void MainWindow::setCustomChrome(bool enabled) {
    if (m_customChrome == enabled)
        return;
    // Changing window flags recreates the top-level native window on some
    // platforms. Capture state first; show() is required afterward by Qt.
    const bool visible = isVisible();
    const bool maximized = isMaximized();
    const bool fullscreen = isFullScreen();
    const QRect savedNormal = normalGeometry().isValid() ? normalGeometry() : geometry();

    m_customChrome = enabled;
    m_titleBar->setVisible(enabled);
    setWindowFlag(Qt::FramelessWindowHint, enabled);
    if (visible) {
        if (fullscreen)
            showFullScreen();
        else if (maximized)
            showMaximized();
        else {
            showNormal();
            setGeometry(savedNormal);
        }
    }
    m_titleBar->update();
    updateResizeHandles();
}

void MainWindow::updateResizeHandles() {
    if (m_resizeHandles.size() != 8)
        return;
    const bool active = m_customChrome && !isMaximized() && !isFullScreen();
    constexpr int e = 6;
    constexpr int c = 11;
    const int w = width();
    const int h = height();
    const QRect rects[] = {
        {0, c, e, qMax(0, h - 2 * c)},
        {w - e, c, e, qMax(0, h - 2 * c)},
        {c, 0, qMax(0, w - 2 * c), e},
        {c, h - e, qMax(0, w - 2 * c), e},
        {0, 0, c, c},
        {w - c, 0, c, c},
        {0, h - c, c, c},
        {w - c, h - c, c, c}
    };
    for (int i = 0; i < m_resizeHandles.size(); ++i) {
        auto *handle = m_resizeHandles[i];
        handle->setGeometry(rects[i]);
        handle->setVisible(active);
        if (active)
            handle->raise();
    }
}

void MainWindow::resizeEvent(QResizeEvent *event) {
    QMainWindow::resizeEvent(event);
    updateResizeHandles();
}

void MainWindow::changeEvent(QEvent *event) {
    QMainWindow::changeEvent(event);
    if (event->type() == QEvent::WindowStateChange) {
        if (m_titleBar)
            m_titleBar->update();
        updateResizeHandles();
    }
    if (event->type() == QEvent::ActivationChange && m_titleBar)
        m_titleBar->update();
}

void MainWindow::loadWindowPreferences() {
    QSettings settings(settingsFilePath(), QSettings::IniFormat);
    const QByteArray geometry = settings.value(QStringLiteral("ui/geometry")).toByteArray();
    if (!geometry.isEmpty())
        restoreGeometry(geometry);
}

void MainWindow::saveWindowPreferences() const {
    QSettings settings(settingsFilePath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("ui/geometry"), saveGeometry());
}

void MainWindow::closeEvent(QCloseEvent *event) {
    for (int i = 0; i < m_stack->count(); ++i) {
        if (auto *session = qobject_cast<SessionWidget *>(m_stack->widget(i)))
            session->setSessionActive(false);
    }
    saveWindowPreferences();
    QMainWindow::closeEvent(event);
}
