#include "MainWindow.h"

#include <QtConcurrent/QtConcurrentRun>

#include <QCheckBox>
#include <QComboBox>
#include <QEvent>
#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QFontDatabase>
#include <QHBoxLayout>
#include <QKeyEvent>
#include <QKeySequence>
#include <QLabel>
#include <QLineEdit>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QShortcut>
#include <QStatusBar>
#include <QTextCursor>
#include <QTextDocument>
#include <QVBoxLayout>
#include <QWidget>

namespace {
constexpr int kFlushIntervalMs = 16;     // ~60 terminal UI updates/sec max
constexpr int kAutoScanIntervalMs = 1500;
constexpr int kStatusIntervalMs = 250;
constexpr int kMaxTerminalChars = 2'000'000;
constexpr int kMaxDisplayHistoryBytes = 2 * 1024 * 1024;
constexpr int kMaxCommandHistory = 200;
}

MainWindow::MainWindow(QWidget *parent) : QMainWindow(parent) {
    qRegisterMetaType<SerialSettings>("SerialSettings");
    buildUi();

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
        m_connectedBaud = currentSettings().baudRate;
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
    emit requestStopLog();
    emit requestClose();
    m_serialThread.quit();
    m_serialThread.wait(1500);
}

void MainWindow::buildUi() {
    setWindowTitle(QStringLiteral("term0 0.7.5"));
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
    m_terminal->setFont(QFontDatabase::systemFont(QFontDatabase::FixedFont));

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
    SerialSettings s;
    s.portName = m_ports->currentData().toString();
    if (s.portName.isEmpty())
        s.portName = m_ports->currentText().trimmed();
    s.baudRate = m_baud->currentData().toInt();
    return s;
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
    m_status->setText(QStringLiteral("Opening %1…").arg(settings.portName));
    emit requestOpen(settings);
}

void MainWindow::toggleLog() {
    if (m_logActive) {
        emit requestStopLog();
        return;
    }

    const QString path = QFileDialog::getSaveFileName(
        this,
        QStringLiteral("Save RX text log"),
        QStringLiteral("term0-log.txt"),
        QStringLiteral("Text log (*.txt);;All files (*)"));

    if (path.isEmpty())
        return;

    m_status->setText(QStringLiteral("Starting log…"));
    emit requestStartLog(path);
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
        prefix = QStringLiteral("%1 @ %2 8-N-1  ")
                     .arg(m_connectedPort)
                     .arg(m_connectedBaud);
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
