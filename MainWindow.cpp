#include "MainWindow.h"

#include <QtConcurrent/QtConcurrentRun>

#include <QApplication>
#include <QPalette>
#include <QCheckBox>
#include <QDateTime>
#include <QDialog>
#include <QDialogButtonBox>
#include <QDir>
#include <QComboBox>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFormLayout>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QSettings>
#include <QStandardPaths>
#include <QShortcut>
#include <QStatusBar>
#include <QSpinBox>
#include <QTabWidget>
#include <QTextCursor>
#include <QTextDocument>
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
constexpr qreal kMinTerminalPointSize = 7.0;
constexpr qreal kMaxTerminalPointSize = 32.0;
constexpr qreal kHiddenThemeTerminalBoost = 2.0;

QString settingsFilePath() {
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
    return dir.filePath(QStringLiteral("term0/term0.ini"));
}

#ifdef Q_OS_WIN
void setWindowsDarkTitleBar(QWidget *widget, bool enabled) {
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
}

bool systemUsesDarkWindowChrome() {
    const QColor window = QApplication::palette().color(QPalette::Window);
    return window.lightness() < 128;
}
#else
void setWindowsDarkTitleBar(QWidget *, bool) {}
bool systemUsesDarkWindowChrome() { return false; }
#endif
}

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    m_standardUiFont = QApplication::font();
    qRegisterMetaType<SerialSettings>("SerialSettings");
    buildUi();
    loadPreferences();

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
        updateSessionStatus();
    });
    connect(m_serialWorker, &SerialWorker::closed, this, [this] {
        if (m_connected && m_connectionElapsed.isValid())
            m_lastElapsedMs = m_connectionElapsed.elapsed();
        m_connected = false;
        setConnectedUi(false);
        m_status->setText(QStringLiteral("Disconnected"));
        updateSessionStatus();
    });
    connect(m_serialWorker, &SerialWorker::connectionLost, this,
            [this](const QString &message) {
        if (m_connected && m_connectionElapsed.isValid())
            m_lastElapsedMs = m_connectionElapsed.elapsed();
        m_connected = false;
        setConnectedUi(false);
        m_status->setText(QStringLiteral("Disconnected: %1").arg(message));
        updateSessionStatus();

        // Freeing the native handle is important on Windows. Trigger an
        // immediate rescan instead of waiting for the next 1.5 s tick.
        scanPortsAsync();
    });
    connect(m_serialWorker, &SerialWorker::errorOccurred, this,
            [this](const QString &message) {
        m_status->setText(message);
    });
    connect(m_serialWorker, &SerialWorker::dataReceived,
            this, &MainWindow::queueIncoming,
            Qt::QueuedConnection);
    connect(m_serialWorker, &SerialWorker::bytesWritten, this,
            [this](qint64 count) {
        if (count > 0)
            m_txBytes += static_cast<quint64>(count);
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
}

void MainWindow::buildUi() {
    setWindowTitle(QStringLiteral("term0 0.8.0"));
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
    for (const qint32 rate : {9600, 19200, 38400, 57600, 115200, 230400,
                              460800, 921600}) {
        m_baud->addItem(QString::number(rate), rate);
    }
    m_baud->setCurrentText(QStringLiteral("115200"));

    m_connect = new QPushButton(QStringLiteral("Connect"));
    m_connect->setObjectName(QStringLiteral("connectButton"));
    m_clear = new QPushButton(QStringLiteral("Clear"));
    m_clear->setToolTip(QStringLiteral("Clear terminal (Ctrl+L)"));

    m_rxView = new QComboBox;
    m_rxView->addItem(QStringLiteral("Text"));
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

    root->addLayout(top);
    root->addWidget(m_terminal, 1);
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
}

SerialSettings MainWindow::currentSettings() const {
    SerialSettings s = m_serialOptions;
    s.portName = m_ports->currentData().toString();
    if (s.portName.isEmpty())
        s.portName = m_ports->currentText().trimmed();
    s.baudRate = m_baud->currentData().toInt();
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
    dialog.resize(430, 330);

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

    if (!enabled) {
        setStyleSheet(QString());
        QApplication::setFont(m_standardUiFont);
        setFont(m_standardUiFont);
        for (QWidget *widget : findChildren<QWidget *>())
            widget->setFont(m_standardUiFont);
        applyTerminalFont();
        updateConnectAppearance();
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

    applyTerminalFont();
    updateConnectAppearance();
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

    const auto settings = currentSettings();
    if (settings.portName.isEmpty()) {
        m_status->setText(QStringLiteral("No serial port selected"));
        return;
    }

    m_preferredPort = settings.portName;
    savePreferences();
    m_status->setText(QStringLiteral("Opening %1…").arg(settings.portName));
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

    const QString path = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("Save RX log"),
        defaultName,
        QStringLiteral("Log file (*.txt);;All files (*)"));

    if (path.isEmpty())
        return;

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
    m_terminal->clear();
}

void MainWindow::queueIncoming(const QByteArray &data) {
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
    m_terminal->ensureCursorVisible();
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
        text = QString::fromLatin1(formatHexBytes(m_displayHistory));
    } else if (m_rxView->currentIndex() == 2) {
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
    m_terminal->ensureCursorVisible();
}

void MainWindow::flushTerminal() {
    if (m_rxBuffer.isEmpty())
        return;

    QByteArray chunk;
    chunk.swap(m_rxBuffer);

    if (m_rxView->currentIndex() == 2) {
        appendHexDumpChunk(chunk);
    } else {
        QString text;
        if (m_rxView->currentIndex() == 1) {
            text = QString::fromLatin1(formatHexBytes(chunk));
        } else {
            // Text mode follows the convention used by many serial terminals:
            // display non-printable C0 bytes as '.', while CR/LF/TAB remain intact.
            text = QString::fromUtf8(sanitizeTextBytes(chunk));
        }

        QTextCursor cursor = m_terminal->textCursor();
        cursor.movePosition(QTextCursor::End);
        cursor.insertText(text);
        m_terminal->setTextCursor(cursor);
        m_terminal->ensureCursorVisible();
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

    m_sessionStats->setText(
        QStringLiteral("%1%2  RX %3 B  TX %4 B%5")
            .arg(prefix, formatElapsed(elapsedMs))
            .arg(m_rxBytes)
            .arg(m_txBytes)
            .arg(log));
}
