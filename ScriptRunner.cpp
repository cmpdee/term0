#include "ScriptRunner.h"

#include <QFile>
#include <QFileDialog>
#include <QFileInfo>
#include <QHBoxLayout>
#include <QLabel>
#include <QPlainTextEdit>
#include <QPushButton>
#include <QRegularExpression>
#include <QTextStream>
#include <QVBoxLayout>

#include <utility>

namespace {
constexpr int kMaxExpandedCommands = 10000;
constexpr int kMaxRxWindow = 64 * 1024;

QString stripComment(const QString &line) {
    bool quoted = false;
    bool escaped = false;
    for (qsizetype i = 0; i < line.size(); ++i) {
        const QChar c = line.at(i);
        if (escaped) {
            escaped = false;
            continue;
        }
        if (c == QLatin1Char('\\')) {
            escaped = true;
            continue;
        }
        if (c == QLatin1Char('"')) {
            quoted = !quoted;
            continue;
        }
        if (c == QLatin1Char('#') && !quoted)
            return line.left(i);
    }
    return line;
}

bool specialKeyBytes(const QString &name, QByteArray &bytes, bool &useLineEnding) {
    const QString key = name.trimmed().toLower();
    bytes.clear();
    useLineEnding = false;

    if (key == QStringLiteral("esc")) bytes = QByteArray(1, '\x1b');
    else if (key == QStringLiteral("up")) bytes = QByteArray("\x1b[A", 3);
    else if (key == QStringLiteral("down")) bytes = QByteArray("\x1b[B", 3);
    else if (key == QStringLiteral("right")) bytes = QByteArray("\x1b[C", 3);
    else if (key == QStringLiteral("left")) bytes = QByteArray("\x1b[D", 3);
    else if (key == QStringLiteral("home")) bytes = QByteArray("\x1b[H", 3);
    else if (key == QStringLiteral("end")) bytes = QByteArray("\x1b[F", 3);
    else if (key == QStringLiteral("pgup") || key == QStringLiteral("pageup"))
        bytes = QByteArray("\x1b[5~", 4);
    else if (key == QStringLiteral("pgdn") || key == QStringLiteral("pagedown"))
        bytes = QByteArray("\x1b[6~", 4);
    else if (key == QStringLiteral("delete") || key == QStringLiteral("del"))
        bytes = QByteArray("\x1b[3~", 4);
    else if (key == QStringLiteral("backspace") || key == QStringLiteral("bs"))
        bytes = QByteArray(1, '\x7f');
    else if (key == QStringLiteral("tab")) bytes = QByteArray(1, '\x09');
    else if (key == QStringLiteral("ctrl+c")) bytes = QByteArray(1, '\x03');
    else if (key == QStringLiteral("ctrl+d")) bytes = QByteArray(1, '\x04');
    else if (key == QStringLiteral("ctrl+z")) bytes = QByteArray(1, '\x1a');
    else if (key == QStringLiteral("ctrl+\\")) bytes = QByteArray(1, '\x1c');
    else if (key == QStringLiteral("enter") || key == QStringLiteral("return"))
        useLineEnding = true;
    else
        return false;
    return true;
}
}

ScriptRunnerDialog::ScriptRunnerDialog(QWidget *parent) : ChromeDialog(parent) {
    setWindowTitle(QStringLiteral("Script Runner"));
    resize(650, 480);

    auto *root = new QVBoxLayout(this);
    m_editor = new QPlainTextEdit(this);
    m_editor->setPlaceholderText(QStringLiteral(
        "# term0 script\n"
        "sendlines 500 {\n"
        "version\n"
        "status\n"
        "}\n"
        "key ctrl+c\n"
        "expect \"READY\" timeout 3000\n"
        "repeat 3 {\n"
        "    sendln \"status\"\n"
        "    sleep 500\n"
        "}"));
    root->addWidget(m_editor, 1);

    m_status = new QLabel(QStringLiteral("Ready"), this);
    root->addWidget(m_status);

    auto *buttons = new QHBoxLayout;
    m_open = new QPushButton(QStringLiteral("Open"), this);
    m_save = new QPushButton(QStringLiteral("Save"), this);
    m_run = new QPushButton(QStringLiteral("Run"), this);
    m_stop = new QPushButton(QStringLiteral("Stop"), this);
    m_stop->setEnabled(false);
    buttons->addWidget(m_open);
    buttons->addWidget(m_save);
    buttons->addStretch();
    buttons->addWidget(m_run);
    buttons->addWidget(m_stop);
    root->addLayout(buttons);

    m_timer.setSingleShot(true);
    connect(&m_timer, &QTimer::timeout, this, &ScriptRunnerDialog::waitTimedOut);
    connect(m_open, &QPushButton::clicked, this, &ScriptRunnerDialog::openScript);
    connect(m_save, &QPushButton::clicked, this, &ScriptRunnerDialog::saveScript);
    connect(m_run, &QPushButton::clicked, this, &ScriptRunnerDialog::runScript);
    connect(m_stop, &QPushButton::clicked, this, &ScriptRunnerDialog::stopScript);
}


void ScriptRunnerDialog::setInitialPath(const QString &path) {
    m_path = path;
    updateTitle();
}

void ScriptRunnerDialog::setSessionLabel(const QString &label) {
    m_sessionLabel = label;
    updateTitle();
}

void ScriptRunnerDialog::updateTitle() {
    QString caption = QStringLiteral("Script Runner");
    if (!m_sessionLabel.isEmpty())
        caption += QStringLiteral(" — %1").arg(m_sessionLabel);
    if (!m_path.isEmpty())
        caption += QStringLiteral(" — %1").arg(QFileInfo(m_path).fileName());
    setWindowTitle(caption);
}

void ScriptRunnerDialog::setConnected(bool connected) {
    m_connected = connected;
    m_run->setEnabled(connected && !m_running);
    if (!connected && m_running)
        fail(QStringLiteral("Stopped: serial port disconnected"));
}

void ScriptRunnerDialog::feedRx(const QByteArray &data) {
    if (!m_running || !m_waitingForExpect || data.isEmpty())
        return;

    m_rxWindow += data;
    if (m_rxWindow.size() > kMaxRxWindow)
        m_rxWindow.remove(0, m_rxWindow.size() - kMaxRxWindow);

    if (!m_rxWindow.contains(m_expectNeedle))
        return;

    m_timer.stop();
    m_waitingForExpect = false;
    m_rxWindow.clear();
    ++m_index;
    QTimer::singleShot(0, this, &ScriptRunnerDialog::runNext);
}

void ScriptRunnerDialog::openScript() {
    const QString path = QFileDialog::getOpenFileName(
        this, QStringLiteral("Open term0 script"), m_path,
        QStringLiteral("term0 script (*.t0s *.txt);;All files (*)"));
    if (path.isEmpty())
        return;

    QFile file(path);
    if (!file.open(QIODevice::ReadOnly | QIODevice::Text)) {
        m_status->setText(QStringLiteral("Open failed: %1").arg(file.errorString()));
        return;
    }
    m_editor->setPlainText(QString::fromUtf8(file.readAll()));
    m_path = path;
    emit pathChanged(path);
    updateTitle();
    m_status->setText(QStringLiteral("Loaded %1").arg(QFileInfo(path).fileName()));
}

void ScriptRunnerDialog::saveScript() {
    QString path = m_path;
    if (path.isEmpty()) {
        path = QFileDialog::getSaveFileName(
            this, QStringLiteral("Save term0 script"), QStringLiteral("script.t0s"),
            QStringLiteral("term0 script (*.t0s);;Text file (*.txt);;All files (*)"));
        if (path.isEmpty())
            return;
    }

    QFile file(path);
    if (!file.open(QIODevice::WriteOnly | QIODevice::Text | QIODevice::Truncate)) {
        m_status->setText(QStringLiteral("Save failed: %1").arg(file.errorString()));
        return;
    }
    const QByteArray data = m_editor->toPlainText().toUtf8();
    if (file.write(data) != data.size()) {
        m_status->setText(QStringLiteral("Save failed: %1").arg(file.errorString()));
        return;
    }
    file.close();
    m_path = path;
    emit pathChanged(path);
    updateTitle();
    m_status->setText(QStringLiteral("Saved %1").arg(QFileInfo(path).fileName()));
}

void ScriptRunnerDialog::runScript() {
    if (!m_connected) {
        m_status->setText(QStringLiteral("Connect a serial port first"));
        return;
    }

    QVector<Command> commands;
    QString error;
    if (!parseScript(commands, error)) {
        m_status->setText(error);
        return;
    }
    if (commands.isEmpty()) {
        m_status->setText(QStringLiteral("Script is empty"));
        return;
    }

    m_commands = std::move(commands);
    m_index = 0;
    m_waitingForExpect = false;
    m_rxWindow.clear();
    setRunning(true);
    runNext();
}

void ScriptRunnerDialog::stopScript() {
    if (!m_running)
        return;
    m_timer.stop();
    m_waitingForExpect = false;
    setRunning(false);
    m_status->setText(QStringLiteral("Stopped"));
}

void ScriptRunnerDialog::runNext() {
    if (!m_running)
        return;
    if (!m_connected) {
        fail(QStringLiteral("Stopped: serial port disconnected"));
        return;
    }
    if (m_index >= m_commands.size()) {
        finish(QStringLiteral("Finished"));
        return;
    }

    const Command &command = m_commands.at(m_index);
    m_status->setText(QStringLiteral("Line %1 / command %2 of %3")
                          .arg(command.sourceLine)
                          .arg(m_index + 1)
                          .arg(m_commands.size()));

    switch (command.type) {
    case CommandType::Send:
        emit sendText(command.text, false);
        ++m_index;
        QTimer::singleShot(0, this, &ScriptRunnerDialog::runNext);
        break;
    case CommandType::SendLine:
        emit sendText(command.text, true);
        ++m_index;
        QTimer::singleShot(0, this, &ScriptRunnerDialog::runNext);
        break;
    case CommandType::Hex:
        emit sendRaw(command.bytes);
        ++m_index;
        QTimer::singleShot(0, this, &ScriptRunnerDialog::runNext);
        break;
    case CommandType::Sleep:
        m_waitingForExpect = false;
        m_timer.start(command.value);
        // The same timer is used for sleep and expect. The timeout handler
        // advances to the next command when this sleep completes.
        break;
    case CommandType::Expect:
        m_waitingForExpect = true;
        m_expectNeedle = command.text.toUtf8();
        m_rxWindow.clear();
        m_status->setText(QStringLiteral("Line %1: waiting for \"%2\"")
                              .arg(command.sourceLine)
                              .arg(command.text));
        m_timer.start(command.timeoutMs);
        break;
    case CommandType::Dtr:
        emit setDtr(command.value != 0);
        ++m_index;
        QTimer::singleShot(0, this, &ScriptRunnerDialog::runNext);
        break;
    case CommandType::Rts:
        emit setRts(command.value != 0);
        ++m_index;
        QTimer::singleShot(0, this, &ScriptRunnerDialog::runNext);
        break;
    }
}

void ScriptRunnerDialog::waitTimedOut() {
    if (!m_running)
        return;
    if (m_waitingForExpect) {
        const Command &command = m_commands.at(m_index);
        fail(QStringLiteral("Line %1: expect timeout").arg(command.sourceLine));
        return;
    }

    ++m_index;
    runNext();
}

void ScriptRunnerDialog::setRunning(bool running) {
    m_running = running;
    m_editor->setReadOnly(running);
    m_open->setEnabled(!running);
    m_save->setEnabled(!running);
    m_run->setEnabled(!running && m_connected);
    m_stop->setEnabled(running);
}

void ScriptRunnerDialog::finish(const QString &message) {
    m_timer.stop();
    m_waitingForExpect = false;
    setRunning(false);
    m_status->setText(message);
}

void ScriptRunnerDialog::fail(const QString &message) {
    m_timer.stop();
    m_waitingForExpect = false;
    setRunning(false);
    m_status->setText(message);
}

bool ScriptRunnerDialog::parseScript(QVector<Command> &commands, QString &error) const {
    const QStringList lines = m_editor->toPlainText().split(QLatin1Char('\n'));
    int index = 0;
    if (!parseBlock(lines, index, commands, error, false))
        return false;
    if (commands.size() > kMaxExpandedCommands) {
        error = QStringLiteral("Script expands to too many commands (max %1)")
                    .arg(kMaxExpandedCommands);
        return false;
    }
    return true;
}

bool ScriptRunnerDialog::parseBlock(const QStringList &lines, int &index,
                                    QVector<Command> &out, QString &error,
                                    bool expectClosingBrace) {
    while (index < lines.size()) {
        const int lineNumber = index + 1;
        QString line = stripComment(lines.at(index)).trimmed();
        ++index;

        if (line.isEmpty())
            continue;
        if (line == QStringLiteral("}")) {
            if (!expectClosingBrace) {
                error = QStringLiteral("Line %1: unexpected '}'").arg(lineNumber);
                return false;
            }
            return true;
        }

        QRegularExpression sendLinesRx(QStringLiteral(R"(^sendlines\s+(\d+)\s*\{\s*$)"),
                                       QRegularExpression::CaseInsensitiveOption);
        const auto sendLinesMatch = sendLinesRx.match(line);
        if (sendLinesMatch.hasMatch()) {
            bool ok = false;
            const int delayMs = sendLinesMatch.captured(1).toInt(&ok);
            if (!ok || delayMs < 0 || delayMs > 86'400'000) {
                error = QStringLiteral("Line %1: sendlines delay expects milliseconds")
                            .arg(lineNumber);
                return false;
            }

            QVector<Command> linesToSend;
            bool closed = false;
            while (index < lines.size()) {
                const int rawLineNumber = index + 1;
                QString rawLine = lines.at(index);
                ++index;
                if (rawLine.endsWith(QLatin1Char('\r')))
                    rawLine.chop(1);
                if (rawLine.trimmed() == QStringLiteral("}")) {
                    closed = true;
                    break;
                }
                if (rawLine.trimmed().isEmpty())
                    continue;

                Command sendCommand;
                sendCommand.type = CommandType::SendLine;
                sendCommand.text = rawLine;
                sendCommand.sourceLine = rawLineNumber;
                linesToSend.append(sendCommand);
            }

            if (!closed) {
                error = QStringLiteral("Line %1: sendlines is missing closing '}'")
                            .arg(lineNumber);
                return false;
            }

            for (qsizetype n = 0; n < linesToSend.size(); ++n) {
                out.append(linesToSend.at(n));
                if (delayMs > 0 && n + 1 < linesToSend.size()) {
                    Command delayCommand;
                    delayCommand.type = CommandType::Sleep;
                    delayCommand.value = delayMs;
                    delayCommand.sourceLine = linesToSend.at(n).sourceLine;
                    out.append(delayCommand);
                }
                if (out.size() > kMaxExpandedCommands) {
                    error = QStringLiteral("Script expands to too many commands (max %1)")
                                .arg(kMaxExpandedCommands);
                    return false;
                }
            }
            continue;
        }

        QRegularExpression repeatRx(QStringLiteral(R"(^repeat\s+(\d+)\s*\{\s*$)"),
                                    QRegularExpression::CaseInsensitiveOption);
        const auto repeatMatch = repeatRx.match(line);
        if (repeatMatch.hasMatch()) {
            bool ok = false;
            const int count = repeatMatch.captured(1).toInt(&ok);
            if (!ok || count < 1 || count > 10000) {
                error = QStringLiteral("Line %1: repeat count must be 1..10000")
                            .arg(lineNumber);
                return false;
            }
            QVector<Command> block;
            if (!parseBlock(lines, index, block, error, true))
                return false;
            if (block.isEmpty())
                continue;
            if (block.size() * count + out.size() > kMaxExpandedCommands) {
                error = QStringLiteral("Line %1: script expands beyond %2 commands")
                            .arg(lineNumber).arg(kMaxExpandedCommands);
                return false;
            }
            for (int n = 0; n < count; ++n) {
                for (const Command &nested : block)
                    out.append(nested);
            }
            continue;
        }

        const int firstSpace = line.indexOf(QRegularExpression(QStringLiteral("\\s")));
        const QString keyword = (firstSpace < 0 ? line : line.left(firstSpace)).toLower();
        const QString rest = firstSpace < 0 ? QString() : line.mid(firstSpace).trimmed();
        Command command;
        command.sourceLine = lineNumber;

        if (keyword == QStringLiteral("send") || keyword == QStringLiteral("sendln")) {
            QString value, trailing, parseError;
            if (!parseQuoted(rest, value, trailing, parseError) || !trailing.trimmed().isEmpty()) {
                error = QStringLiteral("Line %1: %2").arg(lineNumber).arg(parseError.isEmpty()
                            ? QStringLiteral("unexpected text after string") : parseError);
                return false;
            }
            command.type = keyword == QStringLiteral("send")
                ? CommandType::Send : CommandType::SendLine;
            command.text = value;
        } else if (keyword == QStringLiteral("hex")) {
            QString hexText = rest;
            if (hexText.startsWith(QLatin1Char('"'))) {
                QString quotedHex, trailing, parseError;
                if (!parseQuoted(hexText, quotedHex, trailing, parseError) || !trailing.trimmed().isEmpty()) {
                    error = QStringLiteral("Line %1: %2").arg(lineNumber).arg(parseError);
                    return false;
                }
                hexText = quotedHex;
            }
            QString parseError;
            if (!parseHex(hexText, command.bytes, parseError)) {
                error = QStringLiteral("Line %1: %2").arg(lineNumber).arg(parseError);
                return false;
            }
            command.type = CommandType::Hex;
        } else if (keyword == QStringLiteral("key")) {
            bool useLineEnding = false;
            if (!specialKeyBytes(rest, command.bytes, useLineEnding)) {
                error = QStringLiteral("Line %1: unknown key '%2'")
                            .arg(lineNumber).arg(rest);
                return false;
            }
            if (useLineEnding) {
                command.type = CommandType::SendLine;
                command.text.clear();
            } else {
                command.type = CommandType::Hex;
            }
        } else if (keyword == QStringLiteral("sleep")) {
            bool ok = false;
            const int ms = rest.toInt(&ok);
            if (!ok || ms < 0 || ms > 86'400'000) {
                error = QStringLiteral("Line %1: sleep expects milliseconds")
                            .arg(lineNumber);
                return false;
            }
            command.type = CommandType::Sleep;
            command.value = ms;
        } else if (keyword == QStringLiteral("expect")) {
            QString value, trailing, parseError;
            if (!parseQuoted(rest, value, trailing, parseError)) {
                error = QStringLiteral("Line %1: %2").arg(lineNumber).arg(parseError);
                return false;
            }
            if (value.isEmpty()) {
                error = QStringLiteral("Line %1: expect string cannot be empty").arg(lineNumber);
                return false;
            }
            QRegularExpression timeoutRx(QStringLiteral(R"(^\s*timeout\s+(\d+)\s*$)"),
                                         QRegularExpression::CaseInsensitiveOption);
            const auto timeoutMatch = timeoutRx.match(trailing);
            if (!timeoutMatch.hasMatch()) {
                error = QStringLiteral("Line %1: expect requires 'timeout <ms>'")
                            .arg(lineNumber);
                return false;
            }
            bool ok = false;
            const int timeout = timeoutMatch.captured(1).toInt(&ok);
            if (!ok || timeout < 1 || timeout > 86'400'000) {
                error = QStringLiteral("Line %1: invalid expect timeout").arg(lineNumber);
                return false;
            }
            command.type = CommandType::Expect;
            command.text = value;
            command.timeoutMs = timeout;
        } else if (keyword == QStringLiteral("dtr") || keyword == QStringLiteral("rts")) {
            const QString state = rest.toLower();
            if (state != QStringLiteral("on") && state != QStringLiteral("off")) {
                error = QStringLiteral("Line %1: %2 expects on/off")
                            .arg(lineNumber).arg(keyword);
                return false;
            }
            command.type = keyword == QStringLiteral("dtr")
                ? CommandType::Dtr : CommandType::Rts;
            command.value = state == QStringLiteral("on") ? 1 : 0;
        } else {
            error = QStringLiteral("Line %1: unknown command '%2'")
                        .arg(lineNumber).arg(keyword);
            return false;
        }

        out.append(command);
        if (out.size() > kMaxExpandedCommands) {
            error = QStringLiteral("Script expands to too many commands (max %1)")
                        .arg(kMaxExpandedCommands);
            return false;
        }
    }

    if (expectClosingBrace) {
        error = QStringLiteral("Missing closing '}'");
        return false;
    }
    return true;
}

bool ScriptRunnerDialog::parseQuoted(const QString &text, QString &value,
                                     QString &trailing, QString &error) {
    value.clear();
    trailing.clear();
    error.clear();
    if (!text.startsWith(QLatin1Char('"'))) {
        error = QStringLiteral("expected quoted string");
        return false;
    }

    bool escaped = false;
    QString raw;
    qsizetype end = -1;
    for (qsizetype i = 1; i < text.size(); ++i) {
        const QChar c = text.at(i);
        if (escaped) {
            raw += QLatin1Char('\\');
            raw += c;
            escaped = false;
            continue;
        }
        if (c == QLatin1Char('\\')) {
            escaped = true;
            continue;
        }
        if (c == QLatin1Char('"')) {
            end = i;
            break;
        }
        raw += c;
    }
    if (end < 0) {
        error = QStringLiteral("unterminated quoted string");
        return false;
    }

    bool ok = false;
    value = unescapeText(raw, ok, error);
    if (!ok)
        return false;
    trailing = text.mid(end + 1);
    return true;
}

QString ScriptRunnerDialog::unescapeText(const QString &text, bool &ok, QString &error) {
    ok = true;
    error.clear();
    QString out;
    for (qsizetype i = 0; i < text.size(); ++i) {
        QChar c = text.at(i);
        if (c != QLatin1Char('\\')) {
            out += c;
            continue;
        }
        if (++i >= text.size()) {
            ok = false;
            error = QStringLiteral("dangling escape at end of string");
            return {};
        }
        c = text.at(i);
        if (c == QLatin1Char('n')) out += QLatin1Char('\n');
        else if (c == QLatin1Char('r')) out += QLatin1Char('\r');
        else if (c == QLatin1Char('t')) out += QLatin1Char('\t');
        else if (c == QLatin1Char('\\')) out += QLatin1Char('\\');
        else if (c == QLatin1Char('"')) out += QLatin1Char('"');
        else {
            ok = false;
            error = QStringLiteral("unsupported escape \\%1").arg(c);
            return {};
        }
    }
    return out;
}

bool ScriptRunnerDialog::parseHex(const QString &text, QByteArray &data, QString &error) {
    data.clear();
    QString normalized = text.trimmed();
    normalized.replace(QRegularExpression(QStringLiteral("[,;:]")), QStringLiteral(" "));
    QStringList parts = normalized.split(QRegularExpression(QStringLiteral("\\s+")),
                                         Qt::SkipEmptyParts);
    if (parts.isEmpty()) {
        error = QStringLiteral("hex requires at least one byte");
        return false;
    }
    if (parts.size() == 1) {
        QString compact = parts.constFirst();
        if (compact.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
            compact.remove(0, 2);
        if (compact.size() > 2) {
            if (compact.size() % 2 != 0) {
                error = QStringLiteral("HEX input has an odd number of digits");
                return false;
            }
            parts.clear();
            for (qsizetype i = 0; i < compact.size(); i += 2)
                parts << compact.mid(i, 2);
        }
    }
    for (QString token : parts) {
        if (token.startsWith(QStringLiteral("0x"), Qt::CaseInsensitive))
            token.remove(0, 2);
        bool ok = false;
        const uint value = token.toUInt(&ok, 16);
        if (!ok || token.isEmpty() || token.size() > 2 || value > 0xff) {
            error = QStringLiteral("invalid HEX byte '%1'").arg(token);
            return false;
        }
        data.append(static_cast<char>(value));
    }
    return true;
}
