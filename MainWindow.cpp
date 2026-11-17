#include "MainWindow.h"
#include "ScriptRunner.h"
#include "AnsiTerminal.h"

#include <QtConcurrent/QtConcurrentRun>

#include <QApplication>
#include <QClipboard>
#include <QCoreApplication>
#include <QPalette>
#include <QCheckBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QComboBox>
#include <QEvent>
#include <QFile>
#include <QDesktopServices>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QGridLayout>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QIntValidator>
#include <QPlainTextEdit>
#include <QPointer>
#include <QJsonDocument>
#include <QJsonObject>
#include <QPushButton>
#include <QNetworkAccessManager>
#include <QNetworkReply>
#include <QNetworkRequest>
#include <QRegularExpression>
#include <QScrollBar>
#include <QSettings>
#include <QSizePolicy>
#include <QStandardPaths>
#include <QSysInfo>
#include <QShortcut>
#include <QStatusBar>
#include <QSpinBox>
#include <QTabWidget>
#include <QTextCursor>
#include <QTextDocument>
#include <QUrl>
#include <QVersionNumber>
#include <QVBoxLayout>
#include <QWheelEvent>
#include <QWidget>

#ifdef Q_OS_WIN
#include <windows.h>
#include <dwmapi.h>
#endif

namespace {
constexpr int kFlushIntervalMs = 16;     // ~60 terminal UI updates/sec max
constexpr int kAutoScanIntervalMs = 1500;
constexpr int kStatusIntervalMs = 250;
constexpr int kMaxTerminalChars = 2'000'000;
constexpr int kMaxDisplayHistoryBytes = 2 * 1024 * 1024;
constexpr int kMaxCommandHistory = 200;
constexpr qint64 kMaxDebugLogBytes = 2 * 1024 * 1024;
constexpr qreal kMinTerminalPointSize = 7.0;
constexpr qreal kMaxTerminalPointSize = 32.0;
constexpr qreal kHiddenThemeTerminalBoost = 2.0;

QString rxModeId(int index) {
    switch (index) {
    case 1: return QStringLiteral("ansi");
    case 2: return QStringLiteral("hex");
    case 3: return QStringLiteral("hexdump");
    default: return QStringLiteral("text");
    }
}

int rxModeIndex(const QString &id) {
    const QString mode = id.trimmed().toLower();
    if (mode == QStringLiteral("ansi")) return 1;
    if (mode == QStringLiteral("hex")) return 2;
    if (mode == QStringLiteral("hexdump")) return 3;
    return 0;
}

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

    // Creating the native handle here is intentional: DWM attributes are
    // applied to the HWND, not to the Qt widget abstraction.
    HWND hwnd = reinterpret_cast<HWND>(widget->winId());
    if (!hwnd)
        return;

    const BOOL value = enabled ? TRUE : FALSE;
    // 20 is DWMWA_USE_IMMERSIVE_DARK_MODE on current Windows 10/11 builds.
    // Older Windows 10 builds used 19, so fall back to it if needed.
    constexpr DWORD kImmersiveDarkMode = 20;
    constexpr DWORD kImmersiveDarkModeLegacy = 19;
    HRESULT hr = DwmSetWindowAttribute(hwnd, kImmersiveDarkMode,
                                       &value, sizeof(value));
    if (FAILED(hr)) {
        DwmSetWindowAttribute(hwnd, kImmersiveDarkModeLegacy,
                              &value, sizeof(value));
    }

    // A maximized window can keep the old non-client rendering cached until
    // its state changes. Force Windows to repaint the frame immediately.
    SetWindowPos(hwnd, nullptr, 0, 0, 0, 0,
                 SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER |
                 SWP_NOACTIVATE | SWP_FRAMECHANGED);
    RedrawWindow(hwnd, nullptr, nullptr,
                 RDW_INVALIDATE | RDW_FRAME | RDW_UPDATENOW);
    DwmFlush();
}

void setWindowsDarkTitleBar(QWidget *widget, bool enabled) {
    applyWindowsDarkTitleBarOnce(widget, enabled);

    // Qt may still be processing a style/window-state change. Reapply once
    // after control returns to the event loop so maximized windows update too.
    QTimer::singleShot(0, widget, [widget, enabled] {
        applyWindowsDarkTitleBarOnce(widget, enabled);
    });
}

bool systemUsesDarkWindowChrome() {
    const QColor window = QApplication::palette().color(QPalette::Window);
    return window.lightness() < 128;
}
#else
void setWindowsDarkTitleBar(QWidget *, bool) {}
bool systemUsesDarkWindowChrome() { return false; }
#endif

struct TerminalScrollState {
    int horizontal = 0;
    int vertical = 0;
    bool wasAtBottom = true;
};

TerminalScrollState terminalScrollState(QPlainTextEdit *terminal) {
    TerminalScrollState state;
    if (!terminal)
        return state;

    QScrollBar *horizontal = terminal->horizontalScrollBar();
    QScrollBar *vertical = terminal->verticalScrollBar();
    state.horizontal = horizontal->value();
    state.vertical = vertical->value();
    state.wasAtBottom = vertical->value() >= vertical->maximum();
    return state;
}

void restoreTerminalScrollState(QPlainTextEdit *terminal,
                                const TerminalScrollState &state) {
    if (!terminal)
        return;

    QScrollBar *horizontal = terminal->horizontalScrollBar();
    QScrollBar *vertical = terminal->verticalScrollBar();

    // Incoming data should never drag the horizontal viewport to the end of a
    // long line. Preserve whatever horizontal position the user chose.
    horizontal->setValue(qMin(state.horizontal, horizontal->maximum()));

    // Keep following RX only while the user was already at the bottom. If the
    // user scrolls up to inspect old data, do not pull the viewport away.
    if (state.wasAtBottom)
        vertical->setValue(vertical->maximum());
    else
        vertical->setValue(qMin(state.vertical, vertical->maximum()));
}
}

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent),
    m_ansiState(std::make_unique<AnsiTerminalState>()) {
    m_standardUiFont = QApplication::font();
    qRegisterMetaType<SerialSettings>("SerialSettings");
    buildUi();
    loadPreferences();
    if (m_debugEnabled)
        startDebugLog();
    debugLog(QStringLiteral("term0 %1 / Qt %2 / %3")
                 .arg(QCoreApplication::applicationVersion(),
                      QString::fromLatin1(qVersion()),
                      QSysInfo::prettyProductName()));

    m_serialWorker = new SerialWorker;
    m_serialWorker->moveToThread(&m_serialThread);

    connect(&m_serialThread, &QThread::finished,
            m_serialWorker, &QObject::deleteLater);
    connect(this, &MainWindow::requestOpen,
            m_serialWorker, &SerialWorker::openPort,
            Qt::QueuedConnection);
    connect(this, &MainWindow::requestClose,
            m_serialWorker, &SerialWorker::closePort,
            Qt::QueuedConnection);
    connect(this, &MainWindow::requestWrite,
            m_serialWorker, &SerialWorker::writeData,
            Qt::QueuedConnection);
    connect(this, &MainWindow::requestStartLog,
            m_serialWorker, &SerialWorker::startLog,
            Qt::QueuedConnection);
    connect(this, &MainWindow::requestStopLog,
            m_serialWorker, &SerialWorker::stopLog,
            Qt::QueuedConnection);
    connect(this, &MainWindow::requestSetDtr,
            m_serialWorker, &SerialWorker::setDtr,
            Qt::QueuedConnection);
    connect(this, &MainWindow::requestSetRts,
            m_serialWorker, &SerialWorker::setRts,
            Qt::QueuedConnection);
    connect(m_serialWorker, &SerialWorker::diagnostic, this,
            [this](const QString &message) { debugLog(message); });

    connect(m_serialWorker, &SerialWorker::opened, this,
            [this](const QString &name) {
        m_connected = true;
        m_connectedPort = name;
        m_preferredPort = name;
        m_connectedSettings = currentSettings();
        m_connectedBaud = m_connectedSettings.baudRate;
        m_rxBytes = 0;
        m_txBytes = 0;
        m_lastElapsedMs = 0;
        m_connectionElapsed.restart();
        setConnectedUi(true);
        m_status->setText(QStringLiteral("Connected"));
        debugLog(QStringLiteral("Connected: %1").arg(name));
        if (m_scriptRunner)
            m_scriptRunner->setConnected(true);
        updateSessionStatus();
    });
    connect(m_serialWorker, &SerialWorker::closed, this, [this] {
        if (m_connected && m_connectionElapsed.isValid())
            m_lastElapsedMs = m_connectionElapsed.elapsed();
        m_connected = false;
        setConnectedUi(false);
        m_status->setText(QStringLiteral("Disconnected"));
        debugLog(QStringLiteral("Disconnected"));
        if (m_scriptRunner)
            m_scriptRunner->setConnected(false);
        updateSessionStatus();
    });
    connect(m_serialWorker, &SerialWorker::connectionLost, this,
            [this](const QString &message) {
        if (m_connected && m_connectionElapsed.isValid())
            m_lastElapsedMs = m_connectionElapsed.elapsed();
        m_connected = false;
        setConnectedUi(false);
        m_status->setText(QStringLiteral("Disconnected: %1").arg(message));
        debugLog(QStringLiteral("Connection lost: %1").arg(message));
        if (m_scriptRunner)
            m_scriptRunner->setConnected(false);
        updateSessionStatus();

        // Freeing the native handle is important on Windows. Trigger an
        // immediate rescan instead of waiting for the next 1.5 s tick.
        scanPortsAsync();
    });
    connect(m_serialWorker, &SerialWorker::errorOccurred, this,
            [this](const QString &message) {
        m_status->setText(message);
        debugLog(QStringLiteral("Error: %1").arg(message));
    });
    connect(m_serialWorker, &SerialWorker::dataReceived,
            this, &MainWindow::queueIncoming,
            Qt::QueuedConnection);
    connect(m_serialWorker, &SerialWorker::bytesWritten, this,
            [this](qint64 count) {
        if (count > 0) {
            m_txBytes += static_cast<quint64>(count);
            debugLog(QStringLiteral("TX written %1 B").arg(count));
        }
    });
    connect(m_serialWorker, &SerialWorker::logBytesWritten, this,
            [this](qint64 count) {
        if (count > 0)
            m_logBytes += static_cast<quint64>(count);
    });
    connect(m_serialWorker, &SerialWorker::logStateChanged, this,
            [this](bool active, const QString &path) {
        const bool wasActive = m_logActive;
        m_logActive = active;
        m_logPath = path;
        m_log->setText(active ? QStringLiteral("Stop Log")
                              : QStringLiteral("Log"));

        if (active) {
            m_logBytes = 0;
            m_status->setText(QStringLiteral("Logging: %1")
                                  .arg(QFileInfo(path).fileName()));
        } else if (wasActive) {
            m_status->setText(QStringLiteral("Log saved: %1 (%2 B)")
                                  .arg(QFileInfo(path).fileName())
                                  .arg(m_logBytes));
        }
        updateSessionStatus();
    });

    m_serialThread.start();

    connect(&m_scanWatcher,
            &QFutureWatcher<QList<QSerialPortInfo>>::finished,
            this, &MainWindow::applyScannedPorts);

    m_autoScanTimer.setInterval(kAutoScanIntervalMs);
    connect(&m_autoScanTimer, &QTimer::timeout,
            this, &MainWindow::scanPortsAsync);
    m_autoScanTimer.start();

    m_flushTimer.setInterval(kFlushIntervalMs);
    connect(&m_flushTimer, &QTimer::timeout,
            this, &MainWindow::flushTerminal);
    m_flushTimer.start();

    m_statusTimer.setInterval(kStatusIntervalMs);
    connect(&m_statusTimer, &QTimer::timeout,
            this, &MainWindow::updateSessionStatus);
    m_statusTimer.start();

    scanPortsAsync();
}

MainWindow::~MainWindow() {
    savePreferences();
    emit requestStopLog();
    emit requestClose();
    m_serialThread.quit();
    m_serialThread.wait(1500);
    stopDebugLog();
}

void MainWindow::buildUi() {
    setWindowTitle(QStringLiteral("term0 0.9.1"));
    resize(900, 600);

    auto *central = new QWidget(this);
    auto *root = new QVBoxLayout(central);
    auto *top = new QHBoxLayout;

    m_ports = new QComboBox;
    m_ports->setMinimumWidth(260);
    m_ports->setEditable(true);
    m_ports->setInsertPolicy(QComboBox::NoInsert);
    m_ports->setPlaceholderText(QStringLiteral("/dev/ttyUSB0 or COM19"));

    m_baud = new QComboBox;
    m_baud->setEditable(true);
    m_baud->setInsertPolicy(QComboBox::NoInsert);
    m_baud->lineEdit()->setValidator(new QIntValidator(1, 10'000'000, m_baud));
    for (const qint32 rate : {110, 300, 600, 1200, 2400, 4800, 9600, 14400,
                              19200, 28800, 31250, 38400, 56000, 57600, 76800,
                              115200, 128000, 230400, 250000, 256000, 460800,
                              500000, 576000, 921600, 1000000, 1500000,
                              2000000, 3000000}) {
        m_baud->addItem(QString::number(rate), rate);
    }
    m_baud->setCurrentText(QStringLiteral("115200"));
    // Keep the editable baud selector wide enough for common multi-digit
    // values in both the native and alternate UI fonts.
    m_baud->setMinimumContentsLength(8);
    m_baud->setSizeAdjustPolicy(QComboBox::AdjustToMinimumContentsLengthWithIcon);

    m_connect = new QPushButton(QStringLiteral("Connect"));
    m_connect->setObjectName(QStringLiteral("connectButton"));
    m_clear = new QPushButton(QStringLiteral("Clear"));
    m_clear->setToolTip(QStringLiteral("Clear terminal (Ctrl+L)"));

    m_rxView = new QComboBox;
    m_rxView->addItem(QStringLiteral("Text"));
    m_rxView->addItem(QStringLiteral("ANSI"));
    m_rxView->addItem(QStringLiteral("HEX"));
    m_rxView->addItem(QStringLiteral("HEX dump"));
    m_rxView->setToolTip(QStringLiteral("RX display mode"));
    m_rxView->setSizeAdjustPolicy(QComboBox::AdjustToContents);

    m_log = new QPushButton(QStringLiteral("Log"));
    m_log->setToolTip(QStringLiteral("Log new RX text from now; Ctrl+S saves retained RX history"));

    top->addWidget(new QLabel(QStringLiteral("Port:")));
    top->addWidget(m_ports, 1);
    top->addWidget(new QLabel(QStringLiteral("Baud:")));
    top->addWidget(m_baud);
    top->addWidget(m_connect);
    top->addWidget(m_clear);
    top->addWidget(m_rxView);
    top->addWidget(m_log);
    m_terminal = new QPlainTextEdit;
    m_terminal->setReadOnly(true);
    m_terminal->setUndoRedoEnabled(false);
    m_terminal->setLineWrapMode(QPlainTextEdit::NoWrap);

    // Serial output and especially HEX dump columns require a real
    // fixed-width font. Spaces are then deterministic across platforms and
    // keep the ASCII column aligned better than tab stops would.
    QFont terminalFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    m_defaultTerminalPointSize = terminalFont.pointSizeF() > 0
        ? terminalFont.pointSizeF()
        : 10.0;
    m_terminalPointSize = m_defaultTerminalPointSize;
    m_terminal->setFont(terminalFont);
    m_terminal->installEventFilter(this);

    auto *bottom = new QHBoxLayout;
    m_input = new QLineEdit;
    m_send = new QPushButton(QStringLiteral("Send"));
    m_txHex = new QCheckBox(QStringLiteral("HEX"));
    m_txHex->setToolTip(QStringLiteral("Interpret the Send field as hexadecimal bytes"));
    m_cr = new QCheckBox(QStringLiteral("CR"));
    m_lf = new QCheckBox(QStringLiteral("LF"));
    m_cr->setChecked(true);
    m_lf->setChecked(true);

    bottom->addWidget(m_input, 1);
    bottom->addWidget(m_txHex);
    bottom->addWidget(m_cr);
    bottom->addWidget(m_lf);
    bottom->addWidget(m_send);

    // Hidden one-line terminal key strip. Ctrl+Alt+K toggles it without
    // opening another window or changing the normal main-window layout.
    m_keyPanel = new QWidget;
    auto *keyRow = new QHBoxLayout(m_keyPanel);
    keyRow->setContentsMargins(0, 0, 0, 0);
    keyRow->setSpacing(4);

    const auto addKeyButton = [this, keyRow](const QString &label,
                                              const QString &name,
                                              const QString &toolTip) {
        auto *button = new QPushButton(label, m_keyPanel);
        button->setMinimumWidth(0);
        button->setSizePolicy(QSizePolicy::Expanding, QSizePolicy::Fixed);
        button->setToolTip(toolTip);
        keyRow->addWidget(button, 1);
        connect(button, &QPushButton::clicked, this,
                [this, name] { sendSpecialKey(name); });
    };

    addKeyButton(QStringLiteral("Esc"), QStringLiteral("esc"), QStringLiteral("Escape (1B)"));
    addKeyButton(QStringLiteral("←"), QStringLiteral("left"), QStringLiteral("Left"));
    addKeyButton(QStringLiteral("↑"), QStringLiteral("up"), QStringLiteral("Up"));
    addKeyButton(QStringLiteral("↓"), QStringLiteral("down"), QStringLiteral("Down"));
    addKeyButton(QStringLiteral("→"), QStringLiteral("right"), QStringLiteral("Right"));
    addKeyButton(QStringLiteral("Home"), QStringLiteral("home"), QStringLiteral("Home"));
    addKeyButton(QStringLiteral("End"), QStringLiteral("end"), QStringLiteral("End"));
    addKeyButton(QStringLiteral("Pg↑"), QStringLiteral("pgup"), QStringLiteral("Page Up"));
    addKeyButton(QStringLiteral("Pg↓"), QStringLiteral("pgdn"), QStringLiteral("Page Down"));
    addKeyButton(QStringLiteral("Tab"), QStringLiteral("tab"), QStringLiteral("Tab (09)"));
    addKeyButton(QStringLiteral("BS"), QStringLiteral("backspace"), QStringLiteral("Backspace (7F)"));
    addKeyButton(QStringLiteral("Del"), QStringLiteral("delete"), QStringLiteral("Delete"));
    addKeyButton(QStringLiteral("Enter"), QStringLiteral("enter"),
                 QStringLiteral("Enter using current CR/LF settings"));
    addKeyButton(QStringLiteral("^C"), QStringLiteral("ctrl+c"), QStringLiteral("Ctrl+C (03)"));
    addKeyButton(QStringLiteral("^D"), QStringLiteral("ctrl+d"), QStringLiteral("Ctrl+D (04)"));
    addKeyButton(QStringLiteral("^Z"), QStringLiteral("ctrl+z"), QStringLiteral("Ctrl+Z (1A)"));
    addKeyButton(QStringLiteral("^\\"), QStringLiteral("ctrl+\\"), QStringLiteral("Ctrl+\\ (1C)"));
    m_keyPanel->hide();

    root->addLayout(top);
    root->addWidget(m_terminal, 1);
    root->addWidget(m_keyPanel);
    root->addLayout(bottom);
    setCentralWidget(central);

    m_status = new QLabel(QStringLiteral("Disconnected"));
    m_sessionStats = new QLabel(QStringLiteral("00:00:00  RX 0 B  TX 0 B"));
    statusBar()->addWidget(m_status, 1);
    statusBar()->addPermanentWidget(m_sessionStats);

    connect(m_connect, &QPushButton::clicked,
            this, &MainWindow::toggleConnection);
    connect(m_clear, &QPushButton::clicked,
            this, &MainWindow::clearTerminal);
    connect(m_log, &QPushButton::clicked,
            this, &MainWindow::toggleLog);
    connect(m_send, &QPushButton::clicked,
            this, &MainWindow::sendInput);
    connect(m_input, &QLineEdit::returnPressed,
            this, &MainWindow::sendInput);
    connect(m_rxView, &QComboBox::currentIndexChanged,
            this, &MainWindow::renderDisplayHistory);
    connect(m_txHex, &QCheckBox::toggled, this, [this](bool enabled) {
        m_cr->setEnabled(!enabled);
        m_lf->setEnabled(!enabled);
        m_input->setPlaceholderText(
            enabled ? QStringLiteral("HEX bytes, e.g. 04 4D 50 59") : QString());
    });
    m_input->installEventFilter(this);

    auto *clearShortcut = new QShortcut(QKeySequence(QStringLiteral("Ctrl+L")), this);
    connect(clearShortcut, &QShortcut::activated,
            this, &MainWindow::clearTerminal);

    auto *saveShortcut = new QShortcut(QKeySequence::Save, this);
    connect(saveShortcut, &QShortcut::activated,
            this, &MainWindow::saveTextSnapshot);

    auto *refreshShortcut = new QShortcut(QKeySequence(Qt::Key_F5), this);
    connect(refreshShortcut, &QShortcut::activated,
            this, &MainWindow::scanPortsAsync);

    auto *settingsShortcut = new QShortcut(
        QKeySequence(QStringLiteral("Ctrl+Alt+S")), this);
    connect(settingsShortcut, &QShortcut::activated,
            this, &MainWindow::showSerialSettings);

    auto *scriptShortcut = new QShortcut(
        QKeySequence(QStringLiteral("Ctrl+Alt+R")), this);
    connect(scriptShortcut, &QShortcut::activated,
            this, &MainWindow::showScriptRunner);

    auto *keyShortcut = new QShortcut(
        QKeySequence(QStringLiteral("Ctrl+Alt+K")), this);
    connect(keyShortcut, &QShortcut::activated,
            this, &MainWindow::showKeySequences);

    // Ctrl+Alt+T is commonly reserved by Linux desktop environments for
    // launching a terminal. Ctrl+Alt+V is used for the shell-oriented preset.
    auto *terminalModeShortcut = new QShortcut(
        QKeySequence(QStringLiteral("Ctrl+Alt+V")), this);
    connect(terminalModeShortcut, &QShortcut::activated,
            this, &MainWindow::toggleTerminalMode);

    auto *zoomInShortcut = new QShortcut(QKeySequence(QKeySequence::ZoomIn), this);
    connect(zoomInShortcut, &QShortcut::activated, this, [this] {
        adjustTerminalFontSize(1);
    });

    // Some layouts produce '=' for the physical '+' key unless Shift is held.
    auto *zoomInEqualsShortcut = new QShortcut(
        QKeySequence(QStringLiteral("Ctrl+=")), this);
    connect(zoomInEqualsShortcut, &QShortcut::activated, this, [this] {
        adjustTerminalFontSize(1);
    });

    auto *zoomOutShortcut = new QShortcut(QKeySequence(QKeySequence::ZoomOut), this);
    connect(zoomOutShortcut, &QShortcut::activated, this, [this] {
        adjustTerminalFontSize(-1);
    });

    auto *zoomResetShortcut = new QShortcut(
        QKeySequence(QStringLiteral("Ctrl+0")), this);
    connect(zoomResetShortcut, &QShortcut::activated,
            this, &MainWindow::resetTerminalFontSize);

    auto *hiddenThemeShortcut = new QShortcut(
        QKeySequence(QStringLiteral("Ctrl+Alt+0")), this);
    connect(hiddenThemeShortcut, &QShortcut::activated,
            this, &MainWindow::toggleHiddenTheme);

    updateConnectAppearance();
}

QString MainWindow::portLabel(const QSerialPortInfo &info) {
    QString label = info.portName();
    if (!info.description().isEmpty())
        label += QStringLiteral(" — ") + info.description();
    return label;
}

void MainWindow::scanPortsAsync() {
    if (m_scanWatcher.isRunning())
        return;

    // availablePorts() can be slow with some USB/COM drivers. Never run it
    // from the GUI thread.
    m_scanWatcher.setFuture(QtConcurrent::run([] {
        return QSerialPortInfo::availablePorts();
    }));
}

void MainWindow::applyScannedPorts() {
    const auto ports = m_scanWatcher.result();

    QStringList keys;
    keys.reserve(ports.size());
    for (const auto &p : ports)
        keys << p.systemLocation() + QLatin1Char('|') + p.serialNumber();

    if (keys == m_lastPortKeys)
        return;

    bool preferredPortAppeared = false;
    for (int i = 0; i < ports.size(); ++i) {
        if (ports.at(i).portName() == m_preferredPort &&
            !m_lastPortKeys.contains(keys.at(i))) {
            preferredPortAppeared = true;
            break;
        }
    }

    if (m_debugEnabled) {
        for (int i = 0; i < ports.size(); ++i) {
            const auto &port = ports.at(i);
            const QString key = keys.at(i);
            if (!m_lastPortKeys.contains(key)) {
                QString details = QStringLiteral("Port appeared: %1 [%2]")
                    .arg(port.portName(), port.systemLocation());
                if (!port.description().isEmpty())
                    details += QStringLiteral(" desc=\"%1\"").arg(port.description());
                if (!port.manufacturer().isEmpty())
                    details += QStringLiteral(" manufacturer=\"%1\"").arg(port.manufacturer());
                if (!port.serialNumber().isEmpty())
                    details += QStringLiteral(" serial=%1").arg(port.serialNumber());
                if (port.hasVendorIdentifier())
                    details += QStringLiteral(" VID=%1")
                        .arg(port.vendorIdentifier(), 4, 16, QLatin1Char('0'));
                if (port.hasProductIdentifier())
                    details += QStringLiteral(" PID=%1")
                        .arg(port.productIdentifier(), 4, 16, QLatin1Char('0'));
                debugLog(details);
            }
        }
        for (const QString &oldKey : m_lastPortKeys) {
            if (!keys.contains(oldKey))
                debugLog(QStringLiteral("Port removed: %1").arg(oldKey));
        }
    }

    m_lastPortKeys = keys;

    QString restorePort = m_preferredPort;
    if (restorePort.isEmpty()) {
        restorePort = m_ports->currentData().toString();
        if (restorePort.isEmpty())
            restorePort = m_ports->currentText().trimmed();
    }

    m_ports->blockSignals(true);
    m_ports->clear();
    for (const auto &p : ports) {
        m_ports->addItem(portLabel(p), p.portName());
        const int index = m_ports->count() - 1;
        m_ports->setItemData(index, p.systemLocation(), Qt::ToolTipRole);
    }

    const int restoreIndex = m_ports->findData(restorePort);
    if (restoreIndex >= 0)
        m_ports->setCurrentIndex(restoreIndex);
    m_ports->blockSignals(false);

    if (!m_connected && preferredPortAppeared && !m_preferredPort.isEmpty())
        m_status->setText(QStringLiteral("%1 available").arg(m_preferredPort));
}

SerialSettings MainWindow::currentSettings() const {
    SerialSettings s = m_serialOptions;
    const int portIndex = m_ports->currentIndex();
    if (portIndex >= 0 && m_ports->currentText() == m_ports->itemText(portIndex))
        s.portName = m_ports->itemData(portIndex).toString();
    else
        s.portName = m_ports->currentText().trimmed();
    if (s.portName.isEmpty())
        s.portName = m_ports->currentText().trimmed();
    bool baudOk = false;
    const int baud = m_baud->currentText().trimmed().toInt(&baudOk);
    s.baudRate = baudOk && baud > 0 ? baud : QSerialPort::Baud115200;
    return s;
}

void MainWindow::loadPreferences() {
    QSettings settings(settingsFilePath(), QSettings::IniFormat);

    const QString savedPort = settings.value(QStringLiteral("serial/port")).toString();
    if (!savedPort.isEmpty()) {
        m_preferredPort = savedPort;
        m_ports->setEditText(savedPort);
    }

    const qint32 savedBaud = settings.value(
        QStringLiteral("serial/baud"), QSerialPort::Baud115200).toInt();
    int baudIndex = m_baud->findData(savedBaud);
    if (baudIndex < 0) {
        m_baud->addItem(QString::number(savedBaud), savedBaud);
        baudIndex = m_baud->count() - 1;
    }
    m_baud->setCurrentIndex(baudIndex);

    m_serialOptions.dataBits = static_cast<QSerialPort::DataBits>(
        settings.value(QStringLiteral("serial/dataBits"),
                       static_cast<int>(QSerialPort::Data8)).toInt());
    m_serialOptions.parity = static_cast<QSerialPort::Parity>(
        settings.value(QStringLiteral("serial/parity"),
                       static_cast<int>(QSerialPort::NoParity)).toInt());
    m_serialOptions.stopBits = static_cast<QSerialPort::StopBits>(
        settings.value(QStringLiteral("serial/stopBits"),
                       static_cast<int>(QSerialPort::OneStop)).toInt());
    m_serialOptions.flowControl = static_cast<QSerialPort::FlowControl>(
        settings.value(QStringLiteral("serial/flowControl"),
                       static_cast<int>(QSerialPort::NoFlowControl)).toInt());
    m_serialOptions.dataTerminalReady = settings.value(
        QStringLiteral("serial/dtr"), true).toBool();
    m_serialOptions.requestToSend = settings.value(
        QStringLiteral("serial/rts"), false).toBool();

    m_terminalPointSize = settings.value(
        QStringLiteral("ui/terminalFontPointSize"),
        m_defaultTerminalPointSize).toDouble();
    m_terminalPointSize = qBound(kMinTerminalPointSize,
                                 m_terminalPointSize,
                                 kMaxTerminalPointSize);

    m_logFormat = qBound(0, settings.value(
        QStringLiteral("log/format"), 0).toInt(), 2);
    m_logTimestamps = settings.value(
        QStringLiteral("log/timestamps"), false).toBool();
    m_lastLogDirectory = settings.value(QStringLiteral("log/lastDirectory")).toString();
    m_lastScriptPath = settings.value(QStringLiteral("script/lastPath")).toString();
    m_debugEnabled = settings.value(QStringLiteral("debug/enabled"), false).toBool();

    const QString savedRxMode = settings.value(QStringLiteral("ui/rxViewMode")).toString();
    if (!savedRxMode.isEmpty()) {
        m_rxView->setCurrentIndex(rxModeIndex(savedRxMode));
    } else {
        int savedRxView = settings.value(QStringLiteral("ui/rxView"), 0).toInt();
        // Before ANSI mode was added, 1 meant HEX and 2 meant HEX dump.
        if (!settings.value(QStringLiteral("ui/rxViewHasAnsi"), false).toBool()) {
            if (savedRxView == 1)
                savedRxView = 2;
            else if (savedRxView == 2)
                savedRxView = 3;
        }
        m_rxView->setCurrentIndex(qBound(0, savedRxView, 3));
    }
    m_txHex->setChecked(settings.value(QStringLiteral("tx/hex"), false).toBool());
    m_cr->setChecked(settings.value(QStringLiteral("tx/cr"), true).toBool());
    m_lf->setChecked(settings.value(QStringLiteral("tx/lf"), true).toBool());
    const bool txHexEnabled = m_txHex->isChecked();
    m_cr->setEnabled(!txHexEnabled);
    m_lf->setEnabled(!txHexEnabled);
    m_input->setPlaceholderText(txHexEnabled
        ? QStringLiteral("HEX bytes, e.g. 04 4D 50 59") : QString());

    const QByteArray geometry = settings.value(QStringLiteral("ui/geometry")).toByteArray();
    if (!geometry.isEmpty())
        restoreGeometry(geometry);

    applyTerminalFont();
}

void MainWindow::savePreferences() const {
    QSettings settings(settingsFilePath(), QSettings::IniFormat);
    const SerialSettings serial = currentSettings();
    settings.setValue(QStringLiteral("serial/port"), serial.portName);
    settings.setValue(QStringLiteral("serial/baud"), serial.baudRate);
    settings.setValue(QStringLiteral("serial/dataBits"),
                      static_cast<int>(serial.dataBits));
    settings.setValue(QStringLiteral("serial/parity"),
                      static_cast<int>(serial.parity));
    settings.setValue(QStringLiteral("serial/stopBits"),
                      static_cast<int>(serial.stopBits));
    settings.setValue(QStringLiteral("serial/flowControl"),
                      static_cast<int>(serial.flowControl));
    settings.setValue(QStringLiteral("serial/dtr"), serial.dataTerminalReady);
    settings.setValue(QStringLiteral("serial/rts"), serial.requestToSend);
    settings.setValue(QStringLiteral("ui/terminalFontPointSize"),
                      m_terminalPointSize);
    settings.setValue(QStringLiteral("log/format"), m_logFormat);
    settings.setValue(QStringLiteral("log/timestamps"), m_logTimestamps);
    settings.setValue(QStringLiteral("log/lastDirectory"), m_lastLogDirectory);
    settings.setValue(QStringLiteral("script/lastPath"), m_lastScriptPath);
    settings.setValue(QStringLiteral("debug/enabled"), m_debugEnabled);
    const int rxViewToSave = m_terminalMode
        ? m_terminalPreviousRxView : m_rxView->currentIndex();
    const bool txHexToSave = m_terminalMode
        ? m_terminalPreviousTxHex : m_txHex->isChecked();
    const bool crToSave = m_terminalMode
        ? m_terminalPreviousCr : m_cr->isChecked();
    const bool lfToSave = m_terminalMode
        ? m_terminalPreviousLf : m_lf->isChecked();
    settings.setValue(QStringLiteral("ui/rxView"), rxViewToSave);
    settings.setValue(QStringLiteral("ui/rxViewMode"), rxModeId(rxViewToSave));
    settings.setValue(QStringLiteral("ui/rxViewHasAnsi"), true);
    settings.setValue(QStringLiteral("tx/hex"), txHexToSave);
    settings.setValue(QStringLiteral("tx/cr"), crToSave);
    settings.setValue(QStringLiteral("tx/lf"), lfToSave);
    settings.setValue(QStringLiteral("ui/geometry"), saveGeometry());
}


void MainWindow::startDebugLog() {
    if (!m_debugEnabled)
        return;

    if (!m_debugFile)
        m_debugFile = new QFile(debugLogPath(), this);
    if (m_debugFile->isOpen())
        return;

    QFileInfo info(m_debugFile->fileName());
    if (info.exists() && info.size() > kMaxDebugLogBytes) {
        QFile::remove(m_debugFile->fileName() + QStringLiteral(".old"));
        QFile::rename(m_debugFile->fileName(),
                      m_debugFile->fileName() + QStringLiteral(".old"));
    }

    if (!m_debugFile->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text)) {
        m_status->setText(QStringLiteral("Debug log: %1").arg(m_debugFile->errorString()));
        m_debugEnabled = false;
        return;
    }
    debugLog(QStringLiteral("--- diagnostic session started ---"));
}

void MainWindow::stopDebugLog() {
    if (!m_debugFile || !m_debugFile->isOpen())
        return;
    debugLog(QStringLiteral("--- diagnostic session ended ---"));
    m_debugFile->flush();
    m_debugFile->close();
}

void MainWindow::debugLog(const QString &message) {
    if (!m_debugEnabled || !m_debugFile || !m_debugFile->isOpen())
        return;

    if (m_debugFile->size() > kMaxDebugLogBytes) {
        const QString path = m_debugFile->fileName();
        m_debugFile->flush();
        m_debugFile->close();
        QFile::remove(path + QStringLiteral(".old"));
        QFile::rename(path, path + QStringLiteral(".old"));
        m_debugFile->setFileName(path);
        if (!m_debugFile->open(QIODevice::WriteOnly | QIODevice::Append | QIODevice::Text))
            return;
    }

    const QByteArray line = QStringLiteral("%1  %2\n")
        .arg(QDateTime::currentDateTime().toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz")),
             message)
        .toUtf8();
    m_debugFile->write(line);
}

QString MainWindow::debugLogPath() const {
    return QDir(configDirPath()).filePath(QStringLiteral("term0-debug.log"));
}

QString MainWindow::diagnosticsText() const {
    const SerialSettings s = currentSettings();
    QString text = QStringLiteral(
        "term0 %1\n"
        "Qt %2\n"
        "OS: %3\n"
        "CPU: %4\n"
        "Settings: %5\n"
        "Debug log: %6\n"
        "Port: %7\n"
        "Baud: %8\n"
        "Framing: %9\n"
        "Flow control: %10\n"
        "DTR: %11\n"
        "RTS: %12\n");
    text = text.arg(QCoreApplication::applicationVersion());
    text = text.arg(QString::fromLatin1(qVersion()));
    text = text.arg(QSysInfo::prettyProductName());
    text = text.arg(QSysInfo::currentCpuArchitecture());
    text = text.arg(settingsFilePath());
    text = text.arg(debugLogPath());
    text = text.arg(s.portName);
    text = text.arg(s.baudRate);
    text = text.arg(framingSummary());
    text = text.arg(static_cast<int>(s.flowControl));
    text = text.arg(s.dataTerminalReady ? QStringLiteral("on") : QStringLiteral("off"));
    text = text.arg(s.requestToSend ? QStringLiteral("on") : QStringLiteral("off"));
    return text;
}

QString MainWindow::framingSummary() const {
    const SerialSettings &s = m_connected ? m_connectedSettings : m_serialOptions;

    QString dataBits;
    switch (s.dataBits) {
    case QSerialPort::Data5: dataBits = QStringLiteral("5"); break;
    case QSerialPort::Data6: dataBits = QStringLiteral("6"); break;
    case QSerialPort::Data7: dataBits = QStringLiteral("7"); break;
    default: dataBits = QStringLiteral("8"); break;
    }

    QString parity;
    switch (s.parity) {
    case QSerialPort::EvenParity: parity = QStringLiteral("E"); break;
    case QSerialPort::OddParity: parity = QStringLiteral("O"); break;
    case QSerialPort::SpaceParity: parity = QStringLiteral("S"); break;
    case QSerialPort::MarkParity: parity = QStringLiteral("M"); break;
    default: parity = QStringLiteral("N"); break;
    }

    QString stopBits;
    switch (s.stopBits) {
    case QSerialPort::TwoStop: stopBits = QStringLiteral("2"); break;
    case QSerialPort::OneAndHalfStop: stopBits = QStringLiteral("1.5"); break;
    default: stopBits = QStringLiteral("1"); break;
    }

    return QStringLiteral("%1-%2-%3").arg(dataBits, parity, stopBits);
}

void MainWindow::showSerialSettings() {
    QDialog dialog(this);
    dialog.setWindowTitle(QStringLiteral("Settings"));
    dialog.setModal(true);
    dialog.resize(500, 410);

    auto *layout = new QVBoxLayout(&dialog);
    auto *tabs = new QTabWidget(&dialog);
    layout->addWidget(tabs, 1);

    // Serial tab ----------------------------------------------------------
    auto *serialPage = new QWidget;
    auto *serialLayout = new QVBoxLayout(serialPage);
    auto *serialForm = new QFormLayout;

    auto *dataBits = new QComboBox;
    dataBits->addItem(QStringLiteral("5"), static_cast<int>(QSerialPort::Data5));
    dataBits->addItem(QStringLiteral("6"), static_cast<int>(QSerialPort::Data6));
    dataBits->addItem(QStringLiteral("7"), static_cast<int>(QSerialPort::Data7));
    dataBits->addItem(QStringLiteral("8"), static_cast<int>(QSerialPort::Data8));

    auto *parity = new QComboBox;
    parity->addItem(QStringLiteral("None"), static_cast<int>(QSerialPort::NoParity));
    parity->addItem(QStringLiteral("Even"), static_cast<int>(QSerialPort::EvenParity));
    parity->addItem(QStringLiteral("Odd"), static_cast<int>(QSerialPort::OddParity));
    parity->addItem(QStringLiteral("Mark"), static_cast<int>(QSerialPort::MarkParity));
    parity->addItem(QStringLiteral("Space"), static_cast<int>(QSerialPort::SpaceParity));

    auto *stopBits = new QComboBox;
    stopBits->addItem(QStringLiteral("1"), static_cast<int>(QSerialPort::OneStop));
    stopBits->addItem(QStringLiteral("1.5"), static_cast<int>(QSerialPort::OneAndHalfStop));
    stopBits->addItem(QStringLiteral("2"), static_cast<int>(QSerialPort::TwoStop));

    auto *flow = new QComboBox;
    flow->addItem(QStringLiteral("None"), static_cast<int>(QSerialPort::NoFlowControl));
    flow->addItem(QStringLiteral("Hardware (RTS/CTS)"),
                  static_cast<int>(QSerialPort::HardwareControl));
    flow->addItem(QStringLiteral("Software (XON/XOFF)"),
                  static_cast<int>(QSerialPort::SoftwareControl));

    auto *dtr = new QCheckBox(QStringLiteral("Enabled"));
    auto *rts = new QCheckBox(QStringLiteral("Enabled"));

    auto setComboData = [](QComboBox *combo, int value) {
        const int index = combo->findData(value);
        if (index >= 0)
            combo->setCurrentIndex(index);
    };

    setComboData(dataBits, static_cast<int>(m_serialOptions.dataBits));
    setComboData(parity, static_cast<int>(m_serialOptions.parity));
    setComboData(stopBits, static_cast<int>(m_serialOptions.stopBits));
    setComboData(flow, static_cast<int>(m_serialOptions.flowControl));
    dtr->setChecked(m_serialOptions.dataTerminalReady);
    rts->setChecked(m_serialOptions.requestToSend);

    auto updateRtsState = [flow, rts] {
        const auto selected = static_cast<QSerialPort::FlowControl>(
            flow->currentData().toInt());
        rts->setEnabled(selected != QSerialPort::HardwareControl);
        rts->setToolTip(selected == QSerialPort::HardwareControl
            ? QStringLiteral("RTS is controlled automatically by hardware flow control")
            : QString());
    };
    updateRtsState();
    connect(flow, &QComboBox::currentIndexChanged, &dialog,
            [updateRtsState](int) { updateRtsState(); });

    serialForm->addRow(QStringLiteral("Data bits:"), dataBits);
    serialForm->addRow(QStringLiteral("Parity:"), parity);
    serialForm->addRow(QStringLiteral("Stop bits:"), stopBits);
    serialForm->addRow(QStringLiteral("Flow control:"), flow);
    serialForm->addRow(QStringLiteral("DTR:"), dtr);
    serialForm->addRow(QStringLiteral("RTS:"), rts);
    serialLayout->addLayout(serialForm);

    if (m_connected) {
        auto *note = new QLabel(QStringLiteral(
            "Disconnect before changing serial parameters."));
        note->setWordWrap(true);
        serialLayout->addWidget(note);
        dataBits->setEnabled(false);
        parity->setEnabled(false);
        stopBits->setEnabled(false);
        flow->setEnabled(false);
        dtr->setEnabled(false);
        rts->setEnabled(false);
    }
    serialLayout->addStretch();
    tabs->addTab(serialPage, QStringLiteral("Serial"));

    // View tab ------------------------------------------------------------
    auto *viewPage = new QWidget;
    auto *viewLayout = new QVBoxLayout(viewPage);
    auto *viewForm = new QFormLayout;
    auto *fontSize = new QSpinBox;
    fontSize->setRange(static_cast<int>(kMinTerminalPointSize),
                       static_cast<int>(kMaxTerminalPointSize));
    fontSize->setSuffix(QStringLiteral(" pt"));
    fontSize->setValue(qRound(m_terminalPointSize));
    viewForm->addRow(QStringLiteral("Terminal font size:"), fontSize);
    viewLayout->addLayout(viewForm);
    viewLayout->addStretch();

    auto *shortcutHint = new QLabel(QStringLiteral("hack the planet: s -> 0"));
    shortcutHint->setAlignment(Qt::AlignRight);
    shortcutHint->setEnabled(false);
    viewLayout->addWidget(shortcutHint);
    tabs->addTab(viewPage, QStringLiteral("View"));

    // Log tab -------------------------------------------------------------
    auto *logPage = new QWidget;
    auto *logLayout = new QVBoxLayout(logPage);
    auto *logForm = new QFormLayout;
    auto *logFormat = new QComboBox;
    logFormat->addItem(QStringLiteral("Text"), 0);
    logFormat->addItem(QStringLiteral("HEX"), 1);
    logFormat->addItem(QStringLiteral("HEX dump"), 2);
    logFormat->setCurrentIndex(qBound(0, m_logFormat, 2));

    auto *timestamps = new QCheckBox(QStringLiteral("Enabled"));
    timestamps->setChecked(m_logTimestamps);

    logForm->addRow(QStringLiteral("Format:"), logFormat);
    logForm->addRow(QStringLiteral("Timestamps:"), timestamps);
    logLayout->addLayout(logForm);

    auto *logNote = new QLabel(QStringLiteral(
        "These options are used when the next live log is started."));
    logNote->setWordWrap(true);
    logLayout->addWidget(logNote);
    logLayout->addStretch();
    tabs->addTab(logPage, QStringLiteral("Log"));

    // Debug tab -----------------------------------------------------------
    auto *debugPage = new QWidget;
    auto *debugLayout = new QVBoxLayout(debugPage);
    auto *debugEnabled = new QCheckBox(QStringLiteral("Enable diagnostic logging"));
    debugEnabled->setChecked(m_debugEnabled);
    debugLayout->addWidget(debugEnabled);

    auto *debugPathLabel = new QLabel(debugLogPath());
    debugPathLabel->setWordWrap(true);
    debugPathLabel->setTextInteractionFlags(Qt::TextSelectableByMouse);
    debugLayout->addWidget(debugPathLabel);

    auto *debugButtons = new QHBoxLayout;
    auto *openDebugFolder = new QPushButton(QStringLiteral("Open log folder"));
    auto *copyDiagnostics = new QPushButton(QStringLiteral("Copy diagnostics"));
    debugButtons->addWidget(openDebugFolder);
    debugButtons->addWidget(copyDiagnostics);
    debugButtons->addStretch();
    debugLayout->addLayout(debugButtons);

    auto *debugNote = new QLabel(QStringLiteral(
        "The diagnostic log records port scans, serial settings, open/close events, "
        "errors, and RX/TX activity. It is separate from the RX log."));
    debugNote->setWordWrap(true);
    debugLayout->addWidget(debugNote);
    debugLayout->addStretch();
    tabs->addTab(debugPage, QStringLiteral("Debug"));

    connect(openDebugFolder, &QPushButton::clicked, &dialog, [this] {
        QDesktopServices::openUrl(QUrl::fromLocalFile(configDirPath()));
    });
    connect(copyDiagnostics, &QPushButton::clicked, &dialog, [this, debugNote] {
        QApplication::clipboard()->setText(diagnosticsText());
        debugNote->setText(QStringLiteral("Diagnostics copied to clipboard."));
    });

    // About tab -----------------------------------------------------------
    auto *aboutPage = new QWidget;
    auto *aboutLayout = new QVBoxLayout(aboutPage);
    auto *versionLabel = new QLabel(QStringLiteral("term0 %1")
        .arg(QCoreApplication::applicationVersion()));
    QFont versionFont = versionLabel->font();
    versionFont.setBold(true);
    versionLabel->setFont(versionFont);
    aboutLayout->addWidget(versionLabel);
    aboutLayout->addWidget(new QLabel(QStringLiteral("Qt %1").arg(QString::fromLatin1(qVersion()))));
    aboutLayout->addWidget(new QLabel(QStringLiteral("BSD 3-Clause License")));

    auto *repo = new QLabel(QStringLiteral(
        "<a href=\"https://github.com/cmpdee/term0\">github.com/cmpdee/term0</a>"));
    repo->setOpenExternalLinks(true);
    aboutLayout->addWidget(repo);

    auto *checkUpdates = new QPushButton(QStringLiteral("Check for updates"));
    auto *openRelease = new QPushButton(QStringLiteral("Open release page"));
    openRelease->setVisible(false);
    auto *updateStatus = new QLabel;
    updateStatus->setWordWrap(true);
    aboutLayout->addWidget(checkUpdates, 0, Qt::AlignLeft);
    aboutLayout->addWidget(openRelease, 0, Qt::AlignLeft);
    aboutLayout->addWidget(updateStatus);
    aboutLayout->addStretch();
    tabs->addTab(aboutPage, QStringLiteral("About"));

    auto *network = new QNetworkAccessManager(&dialog);
    QPointer<QDialog> dialogGuard(&dialog);
    connect(openRelease, &QPushButton::clicked, &dialog, [dialogGuard] {
        if (!dialogGuard)
            return;
        const QString url = dialogGuard->property("releaseUrl").toString();
        if (!url.isEmpty())
            QDesktopServices::openUrl(QUrl(url));
    });
    connect(checkUpdates, &QPushButton::clicked, &dialog,
            [network, checkUpdates, openRelease, updateStatus, dialogGuard] {
        checkUpdates->setEnabled(false);
        openRelease->setVisible(false);
        updateStatus->setText(QStringLiteral("Checking…"));

        QNetworkRequest request{QUrl(QStringLiteral(
            "https://api.github.com/repos/cmpdee/term0/releases/latest"))};
        request.setRawHeader("Accept", "application/vnd.github+json");
        request.setRawHeader("User-Agent", "term0/0.9.1");
        QNetworkReply *reply = network->get(request);
        QObject::connect(reply, &QNetworkReply::finished, reply,
            [reply, checkUpdates, openRelease, updateStatus, dialogGuard] {
            if (!dialogGuard) {
                reply->deleteLater();
                return;
            }
            checkUpdates->setEnabled(true);
            if (reply->error() != QNetworkReply::NoError) {
                updateStatus->setText(QStringLiteral("Update check failed: %1")
                                          .arg(reply->errorString()));
                reply->deleteLater();
                return;
            }

            const QJsonDocument document = QJsonDocument::fromJson(reply->readAll());
            const QJsonObject object = document.object();
            QString tag = object.value(QStringLiteral("tag_name")).toString();
            if (tag.startsWith(QLatin1Char('v'), Qt::CaseInsensitive))
                tag.remove(0, 1);
            const QString releaseUrl = object.value(QStringLiteral("html_url")).toString();
            dialogGuard->setProperty("releaseUrl", releaseUrl);

            const QVersionNumber latest = QVersionNumber::fromString(tag);
            const QVersionNumber current = QVersionNumber::fromString(
                QCoreApplication::applicationVersion());
            if (latest.isNull()) {
                updateStatus->setText(QStringLiteral("Could not read the release version."));
            } else if (QVersionNumber::compare(latest, current) > 0) {
                updateStatus->setText(QStringLiteral("New version available: %1").arg(tag));
                openRelease->setVisible(!releaseUrl.isEmpty());
            } else {
                updateStatus->setText(QStringLiteral("term0 %1 is up to date.")
                                          .arg(QCoreApplication::applicationVersion()));
            }
            reply->deleteLater();
        });
    });

    auto *buttons = new QDialogButtonBox(
        QDialogButtonBox::Ok | QDialogButtonBox::Cancel);
    connect(buttons, &QDialogButtonBox::accepted, &dialog, &QDialog::accept);
    connect(buttons, &QDialogButtonBox::rejected, &dialog, &QDialog::reject);
    layout->addWidget(buttons);

    setWindowsDarkTitleBar(&dialog,
        m_hiddenTheme ? true : systemUsesDarkWindowChrome());

    if (dialog.exec() != QDialog::Accepted)
        return;

    if (!m_connected) {
        m_serialOptions.dataBits = static_cast<QSerialPort::DataBits>(
            dataBits->currentData().toInt());
        m_serialOptions.parity = static_cast<QSerialPort::Parity>(
            parity->currentData().toInt());
        m_serialOptions.stopBits = static_cast<QSerialPort::StopBits>(
            stopBits->currentData().toInt());
        m_serialOptions.flowControl = static_cast<QSerialPort::FlowControl>(
            flow->currentData().toInt());
        m_serialOptions.dataTerminalReady = dtr->isChecked();
        m_serialOptions.requestToSend = rts->isChecked();
    }

    m_terminalPointSize = qBound(kMinTerminalPointSize,
                                 static_cast<qreal>(fontSize->value()),
                                 kMaxTerminalPointSize);
    m_logFormat = logFormat->currentData().toInt();
    m_logTimestamps = timestamps->isChecked();

    const bool debugWasEnabled = m_debugEnabled;
    const bool debugRequested = debugEnabled->isChecked();
    if (debugRequested && !debugWasEnabled) {
        m_debugEnabled = true;
        startDebugLog();
    } else if (!debugRequested && debugWasEnabled) {
        stopDebugLog();
        m_debugEnabled = false;
    }

    applyTerminalFont();
    savePreferences();
    updateSessionStatus();
    m_status->setText(QStringLiteral("Settings saved"));
}

void MainWindow::applyTerminalFont() {
    QFont terminalFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    qreal pointSize = m_terminalPointSize;
    if (m_hiddenTheme)
        pointSize += kHiddenThemeTerminalBoost;
    terminalFont.setPointSizeF(pointSize);
    terminalFont.setBold(false);
    m_terminal->setFont(terminalFont);
}

void MainWindow::adjustTerminalFontSize(int steps) {
    if (steps == 0)
        return;

    m_terminalPointSize = qBound(
        kMinTerminalPointSize,
        m_terminalPointSize + static_cast<qreal>(steps),
        kMaxTerminalPointSize);
    applyTerminalFont();

    QSettings settings(settingsFilePath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("ui/terminalFontPointSize"),
                      m_terminalPointSize);
}

void MainWindow::resetTerminalFontSize() {
    m_terminalPointSize = m_defaultTerminalPointSize;
    applyTerminalFont();

    QSettings settings(settingsFilePath(), QSettings::IniFormat);
    settings.setValue(QStringLiteral("ui/terminalFontPointSize"),
                      m_terminalPointSize);
}

void MainWindow::updateConnectAppearance() {
    // Keep the standard UI fully native. The alternate display uses one
    // application-wide palette, so the connection button needs no special
    // per-state styling there either.
    m_connect->setStyleSheet(QString());
}

void MainWindow::applyHiddenTheme(bool enabled) {
    m_hiddenTheme = enabled;

    setWindowsDarkTitleBar(this,
        enabled ? true : systemUsesDarkWindowChrome());
    if (m_scriptRunner)
        setWindowsDarkTitleBar(m_scriptRunner,
            enabled ? true : systemUsesDarkWindowChrome());

    if (!enabled) {
        setStyleSheet(QString());
        QApplication::setFont(m_standardUiFont);
        setFont(m_standardUiFont);
        for (QWidget *widget : findChildren<QWidget *>())
            widget->setFont(m_standardUiFont);
        if (m_scriptRunner) {
            m_scriptRunner->setStyleSheet(QString());
            m_scriptRunner->setFont(m_standardUiFont);
        }

        applyTerminalFont();
        if (m_rxView->currentIndex() == 1)
            renderDisplayHistory();
        updateConnectAppearance();

        QTimer::singleShot(0, this, [this] {
            if (auto *layout = centralWidget() ? centralWidget()->layout() : nullptr) {
                layout->invalidate();
                layout->activate();
            }
            m_ports->updateGeometry();
            m_baud->updateGeometry();
        });
        return;
    }

    // A deliberately generic retro-terminal theme. It borrows the idea of a
    // dark terminal-like UI without reproducing another application's assets.
    QFont uiFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
    if (m_standardUiFont.pointSizeF() > 0)
        uiFont.setPointSizeF(m_standardUiFont.pointSizeF());
    QApplication::setFont(uiFont);
    setFont(uiFont);
    for (QWidget *widget : findChildren<QWidget *>())
        widget->setFont(uiFont);

    setStyleSheet(QStringLiteral(R"(
        QMainWindow, QWidget {
            background: #070a08;
            color: #7cff8f;
        }
        QLabel, QCheckBox {
            color: #7cff8f;
        }
        QComboBox, QLineEdit, QPlainTextEdit, QSpinBox {
            background: #020403;
            color: #7cff8f;
            border: 1px solid #31533a;
            border-radius: 0;
            padding: 3px;
            selection-background-color: #17351f;
            selection-color: #a7ffb3;
        }
        QComboBox QAbstractItemView {
            background: #020403;
            color: #7cff8f;
            border: 1px solid #31533a;
            selection-background-color: #17351f;
            selection-color: #a7ffb3;
        }
        QPushButton {
            background: #070a08;
            color: #7cff8f;
            border: 1px solid #31533a;
            border-radius: 0;
            padding: 4px 9px;
        }
        QPushButton:hover {
            border-color: #7cff8f;
            background: #0b120d;
        }
        QPushButton:pressed {
            background: #17351f;
        }
        QPushButton:disabled, QComboBox:disabled, QLineEdit:disabled, QSpinBox:disabled,
        QCheckBox:disabled, QLabel:disabled {
            color: #3d7a49;
            border-color: #203825;
        }
        QCheckBox {
            spacing: 5px;
        }
        QCheckBox::indicator {
            width: 12px;
            height: 12px;
            border: 1px solid #31533a;
            background: #020403;
        }
        QCheckBox::indicator:checked {
            background: #7cff8f;
            border-color: #7cff8f;
        }
        QStatusBar {
            background: #020403;
            color: #7cff8f;
            border-top: 1px solid #31533a;
        }
        QStatusBar QLabel {
            background: #020403;
            color: #7cff8f;
        }
        QTabWidget::pane {
            border: 1px solid #31533a;
            background: #070a08;
        }
        QTabBar::tab {
            background: #020403;
            color: #7cff8f;
            border: 1px solid #31533a;
            padding: 5px 12px;
            margin-right: 1px;
        }
        QTabBar::tab:selected {
            background: #17351f;
            border-color: #7cff8f;
        }
        QToolTip {
            background: #020403;
            color: #7cff8f;
            border: 1px solid #31533a;
        }
    )"));

    if (m_scriptRunner) {
        m_scriptRunner->setStyleSheet(styleSheet());
        m_scriptRunner->setFont(uiFont);
    }
    applyTerminalFont();
    if (m_rxView->currentIndex() == 1)
        renderDisplayHistory();
    updateConnectAppearance();

    // Font and stylesheet changes happen in one event turn. Re-run the layout
    // after Qt has applied the new metrics; this prevents the one-frame/first-
    // toggle overlap seen in alternate view after restoring window geometry.
    QTimer::singleShot(0, this, [this] {
        if (auto *layout = centralWidget() ? centralWidget()->layout() : nullptr) {
            layout->invalidate();
            layout->activate();
        }
        m_ports->updateGeometry();
        m_baud->updateGeometry();
    });
}

void MainWindow::showScriptRunner() {
    if (!m_scriptRunner) {
        m_scriptRunner = new ScriptRunnerDialog(this);
        m_scriptRunner->setAttribute(Qt::WA_DeleteOnClose, false);
        m_scriptRunner->setConnected(m_connected);
        m_scriptRunner->setInitialPath(m_lastScriptPath);

        connect(m_scriptRunner, &ScriptRunnerDialog::sendText, this,
                [this](const QString &text, bool appendLineEnding) {
            if (!m_connected)
                return;
            QByteArray data = text.toUtf8();
            if (appendLineEnding) {
                if (m_cr->isChecked())
                    data += '\r';
                if (m_lf->isChecked())
                    data += '\n';
            }
            debugLog(QStringLiteral("Script TX text %1 B").arg(data.size()));
            emit requestWrite(data);
        });
        connect(m_scriptRunner, &ScriptRunnerDialog::sendRaw, this,
                [this](const QByteArray &data) {
            if (!m_connected)
                return;
            debugLog(QStringLiteral("Script TX raw %1 B").arg(data.size()));
            emit requestWrite(data);
        });
        connect(m_scriptRunner, &ScriptRunnerDialog::setDtr,
                this, &MainWindow::requestSetDtr);
        connect(m_scriptRunner, &ScriptRunnerDialog::setRts,
                this, &MainWindow::requestSetRts);
        connect(m_scriptRunner, &ScriptRunnerDialog::pathChanged, this,
                [this](const QString &path) {
            m_lastScriptPath = path;
            savePreferences();
        });
    }

    if (m_hiddenTheme) {
        m_scriptRunner->setStyleSheet(styleSheet());
        QFont uiFont = QFontDatabase::systemFont(QFontDatabase::FixedFont);
        if (m_standardUiFont.pointSizeF() > 0)
            uiFont.setPointSizeF(m_standardUiFont.pointSizeF());
        m_scriptRunner->setFont(uiFont);
    } else {
        m_scriptRunner->setStyleSheet(QString());
        m_scriptRunner->setFont(m_standardUiFont);
    }
    setWindowsDarkTitleBar(m_scriptRunner,
        m_hiddenTheme ? true : systemUsesDarkWindowChrome());
    m_scriptRunner->show();
    m_scriptRunner->raise();
    m_scriptRunner->activateWindow();
}


void MainWindow::sendSpecialKey(const QString &name) {
    if (!m_connected) {
        m_status->setText(QStringLiteral("Connect a serial port first"));
        return;
    }

    const QString key = name.toLower();
    QByteArray data;
    if (key == QStringLiteral("esc")) data = QByteArray(1, '\x1b');
    else if (key == QStringLiteral("up")) data = QByteArray("\x1b[A", 3);
    else if (key == QStringLiteral("down")) data = QByteArray("\x1b[B", 3);
    else if (key == QStringLiteral("right")) data = QByteArray("\x1b[C", 3);
    else if (key == QStringLiteral("left")) data = QByteArray("\x1b[D", 3);
    else if (key == QStringLiteral("home")) data = QByteArray("\x1b[H", 3);
    else if (key == QStringLiteral("end")) data = QByteArray("\x1b[F", 3);
    else if (key == QStringLiteral("pgup")) data = QByteArray("\x1b[5~", 4);
    else if (key == QStringLiteral("pgdn")) data = QByteArray("\x1b[6~", 4);
    else if (key == QStringLiteral("delete")) data = QByteArray("\x1b[3~", 4);
    else if (key == QStringLiteral("backspace")) data = QByteArray(1, '\x7f');
    else if (key == QStringLiteral("tab")) data = QByteArray(1, '\x09');
    else if (key == QStringLiteral("ctrl+c")) data = QByteArray(1, '\x03');
    else if (key == QStringLiteral("ctrl+d")) data = QByteArray(1, '\x04');
    else if (key == QStringLiteral("ctrl+z")) data = QByteArray(1, '\x1a');
    else if (key == QStringLiteral("ctrl+\\")) data = QByteArray(1, '\x1c');
    else if (key == QStringLiteral("enter")) {
        if (m_cr->isChecked()) data += '\r';
        if (m_lf->isChecked()) data += '\n';
    }

    if (data.isEmpty())
        return;

    debugLog(QStringLiteral("Key TX %1: %2 B").arg(name).arg(data.size()));
    emit requestWrite(data);
}

void MainWindow::showKeySequences() {
    if (!m_keyPanel)
        return;

    const bool show = !m_keyPanel->isVisible();
    m_keyPanel->setVisible(show);
    m_status->setText(show
        ? QStringLiteral("Key sequences shown (Ctrl+Alt+K to hide)")
        : QStringLiteral("Key sequences hidden"));

    // Recalculate only the vertical split. The serial controls and terminal
    // geometry otherwise stay exactly as in the normal view.
    if (auto *layout = centralWidget() ? centralWidget()->layout() : nullptr) {
        layout->invalidate();
        layout->activate();
    }
    m_input->setFocus();
}

void MainWindow::toggleTerminalMode() {
    if (!m_keyPanel)
        return;

    if (!m_terminalMode) {
        // Remember the user's normal serial-terminal view so this remains a
        // temporary preset rather than silently changing saved preferences.
        m_terminalPreviousRxView = m_rxView->currentIndex();
        m_terminalPreviousTxHex = m_txHex->isChecked();
        m_terminalPreviousCr = m_cr->isChecked();
        m_terminalPreviousLf = m_lf->isChecked();
        m_terminalMode = true;
        m_rxView->setCurrentIndex(rxModeIndex(QStringLiteral("ansi")));
        m_txHex->setChecked(false);
        m_cr->setChecked(true);
        m_lf->setChecked(false);
        m_keyPanel->show();
        m_status->setText(QStringLiteral(
            "Terminal mode: ANSI + CR + key sequences (Ctrl+Alt+V to exit)"));
    } else {
        m_terminalMode = false;
        m_rxView->setCurrentIndex(qBound(0, m_terminalPreviousRxView,
                                         m_rxView->count() - 1));
        m_txHex->setChecked(m_terminalPreviousTxHex);
        m_cr->setChecked(m_terminalPreviousCr);
        m_lf->setChecked(m_terminalPreviousLf);
        // The key strip is part of Terminal mode. Leaving the mode always
        // returns the main window to its compact normal layout.
        m_keyPanel->hide();
        m_status->setText(QStringLiteral("Terminal mode off"));
    }

    if (auto *layout = centralWidget() ? centralWidget()->layout() : nullptr) {
        layout->invalidate();
        layout->activate();
    }
    m_input->setFocus();
    updateSessionStatus();
}

void MainWindow::toggleHiddenTheme() {
    applyHiddenTheme(!m_hiddenTheme);
    m_status->setText(m_hiddenTheme
        ? QStringLiteral("term0 // alternate display")
        : QStringLiteral("Standard display"));
}

void MainWindow::toggleConnection() {
    if (m_connected) {
        emit requestClose();
        return;
    }

    bool baudOk = false;
    const int typedBaud = m_baud->currentText().trimmed().toInt(&baudOk);
    if (!baudOk || typedBaud <= 0) {
        m_status->setText(QStringLiteral("Invalid baud rate"));
        return;
    }

    const auto settings = currentSettings();
    if (settings.portName.isEmpty()) {
        m_status->setText(QStringLiteral("No serial port selected"));
        return;
    }

    m_preferredPort = settings.portName;
    savePreferences();
    m_status->setText(QStringLiteral("Opening %1…").arg(settings.portName));
    debugLog(QStringLiteral("Connect requested: %1 @ %2 %3")
                 .arg(settings.portName)
                 .arg(settings.baudRate)
                 .arg(framingSummary()));
    emit requestOpen(settings);
}

void MainWindow::toggleLog() {
    if (m_logActive) {
        emit requestStopLog();
        return;
    }

    QString defaultName = QStringLiteral("term0-log");
    if (m_logFormat == 1)
        defaultName += QStringLiteral("_hex");
    else if (m_logFormat == 2)
        defaultName += QStringLiteral("_hexdump");

    if (m_logTimestamps) {
        defaultName += QStringLiteral("_") +
            QDateTime::currentDateTime().toString(QStringLiteral("dd_MM_yyyy_HH_mm"));
    }
    defaultName += QStringLiteral(".txt");

    const QString suggestedPath = m_lastLogDirectory.isEmpty()
        ? defaultName
        : QDir(m_lastLogDirectory).filePath(defaultName);
    const QString path = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("Save RX log"),
        suggestedPath,
        QStringLiteral("Log file (*.txt);;All files (*)"));

    if (path.isEmpty())
        return;

    m_lastLogDirectory = QFileInfo(path).absolutePath();
    savePreferences();
    m_status->setText(QStringLiteral("Starting log…"));
    emit requestStartLog(path, m_logFormat, m_logTimestamps);
}


void MainWindow::saveTextSnapshot() {
    if (m_displayHistory.isEmpty()) {
        m_status->setText(QStringLiteral("Nothing to save"));
        return;
    }

    const QString path = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("Save RX text"),
        QStringLiteral("term0-rx.txt"),
        QStringLiteral("Text file (*.txt);;All files (*)"));

    if (path.isEmpty())
        return;

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        m_status->setText(QStringLiteral("Save failed: %1").arg(file.errorString()));
        return;
    }

    const QByteArray text = sanitizeTextBytes(m_displayHistory);
    const qint64 written = file.write(text);
    if (written != text.size()) {
        m_status->setText(QStringLiteral("Save failed: %1").arg(file.errorString()));
        file.close();
        return;
    }

    file.close();
    m_status->setText(QStringLiteral("Saved RX text: %1 (%2 B)")
                          .arg(QFileInfo(path).fileName())
                          .arg(written));
}

void MainWindow::setConnectedUi(bool connected) {
    m_connect->setText(connected ? QStringLiteral("Disconnect")
                                 : QStringLiteral("Connect"));
    m_ports->setEnabled(!connected);
    m_baud->setEnabled(!connected);
    if (m_scriptRunner)
        m_scriptRunner->setConnected(connected);
    updateConnectAppearance();
}

bool MainWindow::parseHexInput(const QString &text, QByteArray &result,
                               QString &errorMessage) {
    result.clear();
    errorMessage.clear();

    QString normalized = text.trimmed();
    normalized.replace(QRegularExpression(QStringLiteral("[,;:]")),
                       QStringLiteral(" "));
    if (normalized.isEmpty())
        return true;

    QStringList parts = normalized.split(
        QRegularExpression(QStringLiteral("\\s+")), Qt::SkipEmptyParts);

    // Also accept a compact even-length stream such as 044D5059.
    if (parts.size() == 1) {
        QString compact = parts.constFirst();
        if (compact.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
            compact.remove(0, 2);

        if (compact.size() > 2) {
            if ((compact.size() % 2) != 0) {
                errorMessage = QStringLiteral("HEX input has an odd number of digits");
                return false;
            }

            for (qsizetype i = 0; i < compact.size(); i += 2) {
                bool ok = false;
                const uint value = compact.mid(i, 2).toUInt(&ok, 16);
                if (!ok) {
                    errorMessage = QStringLiteral("Invalid HEX byte near '%1'")
                                       .arg(compact.mid(i, 2));
                    return false;
                }
                result.append(static_cast<char>(value));
            }
            return true;
        }
    }

    for (QString token : parts) {
        if (token.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
            token.remove(0, 2);

        if (token.isEmpty() || token.size() > 2) {
            errorMessage = QStringLiteral("Invalid HEX byte '%1'").arg(token);
            return false;
        }

        bool ok = false;
        const uint value = token.toUInt(&ok, 16);
        if (!ok || value > 0xff) {
            errorMessage = QStringLiteral("Invalid HEX byte '%1'").arg(token);
            return false;
        }
        result.append(static_cast<char>(value));
    }

    return true;
}

void MainWindow::sendInput() {
    if (!m_connected)
        return;

    const QString command = m_input->text();
    QByteArray data;

    if (m_txHex->isChecked()) {
        QString errorMessage;
        if (!parseHexInput(command, data, errorMessage)) {
            m_status->setText(errorMessage);
            return;
        }
        if (data.isEmpty())
            return;
    } else {
        data = command.toUtf8();
        if (m_cr->isChecked())
            data += '\r';
        if (m_lf->isChecked())
            data += '\n';
    }

    emit requestWrite(data);

    // Keep shell-like command history only in memory. Do not persist serial
    // commands to disk because they can contain credentials or other secrets.
    if (!command.trimmed().isEmpty() &&
        (m_commandHistory.isEmpty() || m_commandHistory.constLast() != command)) {
        m_commandHistory.append(command);
        if (m_commandHistory.size() > kMaxCommandHistory)
            m_commandHistory.removeFirst();
    }

    m_historyIndex = m_commandHistory.size();
    m_historyDraft.clear();
    m_input->clear();
}

bool MainWindow::eventFilter(QObject *watched, QEvent *event) {
    if (watched == m_terminal && event->type() == QEvent::Wheel) {
        auto *wheelEvent = static_cast<QWheelEvent *>(event);
        if (wheelEvent->modifiers().testFlag(Qt::ControlModifier)) {
            int delta = wheelEvent->angleDelta().y();
            if (delta == 0)
                delta = wheelEvent->pixelDelta().y();
            if (delta != 0) {
                adjustTerminalFontSize(delta > 0 ? 1 : -1);
                return true;
            }
        }
    }

    if (watched == m_input && event->type() == QEvent::KeyPress) {
        auto *keyEvent = static_cast<QKeyEvent *>(event);

        if (keyEvent->key() == Qt::Key_Up && !m_commandHistory.isEmpty()) {
            if (m_historyIndex >= m_commandHistory.size()) {
                m_historyIndex = m_commandHistory.size();
                m_historyDraft = m_input->text();
            }

            if (m_historyIndex > 0)
                --m_historyIndex;

            m_input->setText(m_commandHistory.at(m_historyIndex));
            m_input->setCursorPosition(m_input->text().size());
            return true;
        }

        if (keyEvent->key() == Qt::Key_Down && !m_commandHistory.isEmpty()) {
            if (m_historyIndex < m_commandHistory.size() - 1) {
                ++m_historyIndex;
                m_input->setText(m_commandHistory.at(m_historyIndex));
            } else if (m_historyIndex < m_commandHistory.size()) {
                m_historyIndex = m_commandHistory.size();
                m_input->setText(m_historyDraft);
            }

            m_input->setCursorPosition(m_input->text().size());
            return true;
        }
    }

    return QMainWindow::eventFilter(watched, event);
}

void MainWindow::clearTerminal() {
    m_rxBuffer.clear();
    m_displayHistory.clear();
    m_displayHistoryBaseOffset = 0;
    m_dumpTail.clear();
    m_dumpNextOffset = 0;
    m_dumpPartialChars = 0;
    if (m_ansiState)
        m_ansiState->reset();
    m_terminal->clear();
}

void MainWindow::queueIncoming(const QByteArray &data) {
    if (m_scriptRunner)
        m_scriptRunner->feedRx(data);
    debugLog(QStringLiteral("RX %1 B").arg(data.size()));

    // Deliberately do not touch the text widget on every readyRead().
    m_rxBytes += static_cast<quint64>(data.size());
    m_rxBuffer += data;
    m_displayHistory += data;

    if (m_displayHistory.size() > kMaxDisplayHistoryBytes) {
        const qsizetype excess = m_displayHistory.size() - kMaxDisplayHistoryBytes;
        m_displayHistory.remove(0, excess);
        m_displayHistoryBaseOffset += static_cast<quint64>(excess);
    }
}

QByteArray MainWindow::sanitizeTextBytes(const QByteArray &data) {
    QByteArray result = data;
    for (char &value : result) {
        const auto byte = static_cast<unsigned char>(value);
        const bool c0Control = byte < 0x20 && byte != '\r' &&
                               byte != '\n' && byte != '\t';
        if (c0Control || byte == 0x7f)
            value = '.';
    }
    return result;
}

QByteArray MainWindow::formatHexBytes(const QByteArray &data) {
    static constexpr char kHex[] = "0123456789ABCDEF";

    // Plain HEX is line-oriented rather than fixed-width. Every byte is
    // still shown, including CR/LF; an LF byte (0A) ends the displayed row.
    // Fixed 16-byte rows with offsets belong to the separate HEX dump mode.
    QByteArray output;
    output.reserve(data.size() * 3 + 2);

    for (const char value : data) {
        const auto byte = static_cast<unsigned char>(value);
        output.append(kHex[(byte >> 4) & 0x0f]);
        output.append(kHex[byte & 0x0f]);

        if (byte == 0x0a)
            output.append('\n');
        else
            output.append(' ');
    }

    return output;
}

QByteArray MainWindow::formatHexDumpLine(const QByteArray &data,
                                         quint64 offset,
                                         bool appendNewline) {
    static constexpr char kHex[] = "0123456789ABCDEF";

    QByteArray output = QByteArray::number(offset, 16).toUpper().rightJustified(8, '0');
    output.append("  ");

    for (int i = 0; i < 16; ++i) {
        if (i < data.size()) {
            const auto byte = static_cast<unsigned char>(data.at(i));
            output.append(kHex[(byte >> 4) & 0x0f]);
            output.append(kHex[byte & 0x0f]);
        } else {
            output.append("  ");
        }

        output.append(' ');
        if (i == 7)
            output.append(' ');
    }

    output.append(" |");
    for (const char value : data) {
        const auto byte = static_cast<unsigned char>(value);
        output.append(byte >= 0x20 && byte <= 0x7e
                          ? static_cast<char>(byte)
                          : '.');
    }
    output.append('|');

    if (appendNewline)
        output.append('\n');

    return output;
}

QByteArray MainWindow::formatHexDump(const QByteArray &data,
                                     quint64 startOffset) {
    QByteArray output;
    output.reserve((data.size() / 16 + 1) * 78);

    qsizetype position = 0;
    while (position < data.size()) {
        const qsizetype count = qMin<qsizetype>(16, data.size() - position);
        const QByteArray line = data.mid(position, count);
        const bool fullLine = count == 16;
        output += formatHexDumpLine(
            line, startOffset + static_cast<quint64>(position), fullLine);
        position += count;
    }

    return output;
}

void MainWindow::appendHexDumpChunk(const QByteArray &data) {
    const TerminalScrollState scrollState = terminalScrollState(m_terminal);
    QTextCursor cursor = m_terminal->textCursor();
    cursor.movePosition(QTextCursor::End);

    // A short final line was shown on the previous UI flush. Remove it,
    // extend it with the newly arrived bytes, and draw it again. Complete
    // 16-byte lines are never redrawn.
    if (m_dumpPartialChars > 0) {
        cursor.movePosition(QTextCursor::Left,
                            QTextCursor::KeepAnchor,
                            m_dumpPartialChars);
        cursor.removeSelectedText();
        m_dumpPartialChars = 0;
    }

    m_dumpTail += data;
    QByteArray output;

    while (m_dumpTail.size() >= 16) {
        const QByteArray line = m_dumpTail.left(16);
        output += formatHexDumpLine(line, m_dumpNextOffset, true);
        m_dumpTail.remove(0, 16);
        m_dumpNextOffset += 16;
    }

    if (!m_dumpTail.isEmpty()) {
        const QByteArray partial = formatHexDumpLine(
            m_dumpTail, m_dumpNextOffset, false);
        m_dumpPartialChars = partial.size();
        output += partial;
    }

    cursor.insertText(QString::fromLatin1(output));
    m_terminal->setTextCursor(cursor);
    restoreTerminalScrollState(m_terminal, scrollState);
}

void MainWindow::renderDisplayHistory() {
    m_terminal->clear();
    m_dumpTail.clear();
    m_dumpNextOffset = m_displayHistoryBaseOffset;
    m_dumpPartialChars = 0;

    // m_displayHistory already includes bytes still waiting in m_rxBuffer.
    // Clearing the pending buffer prevents those bytes being appended twice
    // immediately after a view-mode switch.
    m_rxBuffer.clear();

    if (m_displayHistory.isEmpty())
        return;

    QString text;
    if (m_rxView->currentIndex() == 1) {
        m_ansiState->reset();
        m_ansiState->feed(m_displayHistory);
        m_ansiState->render(m_terminal, true);
        return;
    } else if (m_rxView->currentIndex() == 2) {
        text = QString::fromLatin1(formatHexBytes(m_displayHistory));
    } else if (m_rxView->currentIndex() == 3) {
        text = QString::fromLatin1(
            formatHexDump(m_displayHistory, m_displayHistoryBaseOffset));

        const qsizetype fullBytes = (m_displayHistory.size() / 16) * 16;
        m_dumpNextOffset = m_displayHistoryBaseOffset +
                           static_cast<quint64>(fullBytes);
        m_dumpTail = m_displayHistory.mid(fullBytes);
        if (!m_dumpTail.isEmpty()) {
            m_dumpPartialChars = formatHexDumpLine(
                m_dumpTail, m_dumpNextOffset, false).size();
        }
    } else {
        text = QString::fromUtf8(sanitizeTextBytes(m_displayHistory));
    }

    m_terminal->setPlainText(text);
    QTextCursor cursor = m_terminal->textCursor();
    cursor.movePosition(QTextCursor::End);
    m_terminal->setTextCursor(cursor);
    m_terminal->horizontalScrollBar()->setValue(0);
    m_terminal->verticalScrollBar()->setValue(
        m_terminal->verticalScrollBar()->maximum());
}

void MainWindow::flushTerminal() {
    if (m_rxBuffer.isEmpty())
        return;

    QByteArray chunk;
    chunk.swap(m_rxBuffer);

    if (m_rxView->currentIndex() == 3) {
        appendHexDumpChunk(chunk);
    } else if (m_rxView->currentIndex() == 1) {
        m_ansiState->feed(chunk);
        m_ansiState->render(m_terminal);
    } else {
        QString text;
        if (m_rxView->currentIndex() == 2) {
            text = QString::fromLatin1(formatHexBytes(chunk));
        } else {
            // Text mode follows the convention used by many serial terminals:
            // display non-printable C0 bytes as '.', while CR/LF/TAB remain intact.
            text = QString::fromUtf8(sanitizeTextBytes(chunk));
        }

        const TerminalScrollState scrollState = terminalScrollState(m_terminal);
        QTextCursor cursor = m_terminal->textCursor();
        cursor.movePosition(QTextCursor::End);
        cursor.insertText(text);
        m_terminal->setTextCursor(cursor);
        restoreTerminalScrollState(m_terminal, scrollState);
    }

    // Protect the UI from an endlessly growing terminal buffer, including
    // streams without newline characters.
    const int excess = m_terminal->document()->characterCount() - kMaxTerminalChars;
    if (excess > 0) {
        QTextCursor trim(m_terminal->document());
        trim.setPosition(0);
        trim.setPosition(excess, QTextCursor::KeepAnchor);
        trim.removeSelectedText();
    }
}

QString MainWindow::formatElapsed(qint64 milliseconds) {
    qint64 totalSeconds = milliseconds / 1000;
    const qint64 hours = totalSeconds / 3600;
    totalSeconds %= 3600;
    const qint64 minutes = totalSeconds / 60;
    const qint64 seconds = totalSeconds % 60;
    return QStringLiteral("%1:%2:%3")
        .arg(hours, 2, 10, QLatin1Char('0'))
        .arg(minutes, 2, 10, QLatin1Char('0'))
        .arg(seconds, 2, 10, QLatin1Char('0'));
}

void MainWindow::updateSessionStatus() {
    if (m_debugFile && m_debugFile->isOpen())
        m_debugFile->flush();

    qint64 elapsedMs = m_lastElapsedMs;
    if (m_connected && m_connectionElapsed.isValid())
        elapsedMs = m_connectionElapsed.elapsed();

    QString prefix;
    if (m_connected) {
        prefix = QStringLiteral("%1 @ %2 %3  ")
                     .arg(m_connectedPort)
                     .arg(m_connectedBaud)
                     .arg(framingSummary());
    }

    QString log;
    if (m_logActive)
        log = QStringLiteral("  LOG %1 B").arg(m_logBytes);
    const QString terminal = m_terminalMode ? QStringLiteral("  TERM") : QString();
    const QString debug = m_debugEnabled ? QStringLiteral("  DBG") : QString();

    m_sessionStats->setText(
        QStringLiteral("%1%2  RX %3 B  TX %4 B%5%6%7")
            .arg(prefix, formatElapsed(elapsedMs))
            .arg(m_rxBytes)
            .arg(m_txBytes)
            .arg(log)
            .arg(terminal)
            .arg(debug));
}
