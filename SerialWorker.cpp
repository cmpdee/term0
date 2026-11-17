#include "SerialWorker.h"

#include <QDateTime>
#include <QFile>

SerialWorker::SerialWorker(QObject *parent) : QObject(parent) {}

SerialWorker::~SerialWorker() {
    flushPendingLogData();
    closeLogFile();

    if (m_port) {
        if (m_port->isOpen())
            m_port->close();
        delete m_port;
        m_port = nullptr;
    }
}

QByteArray SerialWorker::sanitizeTextBytes(const QByteArray &data) {
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

QByteArray SerialWorker::formatHexDumpLine(const QByteArray &data,
                                           quint64 offset) {
    QByteArray out = QByteArray::number(offset, 16).rightJustified(8, '0').toUpper();
    out += "  ";

    for (int i = 0; i < 16; ++i) {
        if (i < data.size()) {
            const unsigned char byte = static_cast<unsigned char>(data.at(i));
            out += QByteArray::number(byte, 16).rightJustified(2, '0').toUpper();
        } else {
            out += "  ";
        }
        out += (i == 7 ? "  " : " ");
    }

    out += " |";
    for (char value : data) {
        const unsigned char byte = static_cast<unsigned char>(value);
        out += (byte >= 0x20 && byte <= 0x7e) ? static_cast<char>(byte) : '.';
    }
    out += QByteArray(16 - data.size(), ' ');
    out += '|';
    return out;
}

QByteArray SerialWorker::timestampPrefix() const {
    if (!m_logTimestamps)
        return {};
    return QDateTime::currentDateTime()
        .toString(QStringLiteral("yyyy-MM-dd HH:mm:ss.zzz "))
        .toUtf8();
}

bool SerialWorker::writeLogData(const QByteArray &data) {
    if (!m_logFile || !m_logFile->isOpen() || data.isEmpty())
        return true;

    const qint64 written = m_logFile->write(data);
    if (written > 0)
        emit logBytesWritten(written);

    if (written == data.size())
        return true;

    const QString path = m_logPath;
    const QString message = QStringLiteral("Log write failed: %1")
                                .arg(m_logFile->errorString());
    closeLogFile();
    emit logStateChanged(false, path);
    emit errorOccurred(message);
    return false;
}

void SerialWorker::writeLogChunk(const QByteArray &chunk) {
    if (!m_logFile || !m_logFile->isOpen() || chunk.isEmpty())
        return;

    // Text
    if (m_logFormat == 0) {
        const QByteArray text = sanitizeTextBytes(chunk);
        if (!m_logTimestamps) {
            writeLogData(text);
            return;
        }

        QByteArray out;
        for (char value : text) {
            if (m_logLineStart) {
                out += timestampPrefix();
                m_logLineStart = false;
            }
            out += value;
            if (value == '\n')
                m_logLineStart = true;
        }
        writeLogData(out);
        return;
    }

    // HEX: follow the on-screen HEX convention and start a new row after 0A.
    if (m_logFormat == 1) {
        QByteArray out;
        for (char value : chunk) {
            if (m_logLineStart) {
                out += timestampPrefix();
                m_logLineStart = false;
            }
            const unsigned char byte = static_cast<unsigned char>(value);
            out += QByteArray::number(byte, 16).rightJustified(2, '0').toUpper();
            if (byte == 0x0a) {
                out += '\n';
                m_logLineStart = true;
            } else {
                out += ' ';
            }
        }
        writeLogData(out);
        return;
    }

    // HEX dump: complete rows are written immediately. A final partial row is
    // flushed when logging stops.
    m_logDumpTail += chunk;
    while (m_logDumpTail.size() >= 16) {
        const QByteArray line = m_logDumpTail.left(16);
        m_logDumpTail.remove(0, 16);

        QByteArray out = timestampPrefix();
        out += formatHexDumpLine(line, m_logDumpOffset);
        out += '\n';
        if (!writeLogData(out))
            return;
        m_logDumpOffset += 16;
    }
}

void SerialWorker::flushPendingLogData() {
    if (!m_logFile || !m_logFile->isOpen())
        return;

    if (m_logFormat == 2 && !m_logDumpTail.isEmpty()) {
        QByteArray out = timestampPrefix();
        out += formatHexDumpLine(m_logDumpTail, m_logDumpOffset);
        out += '\n';
        writeLogData(out);
        m_logDumpOffset += static_cast<quint64>(m_logDumpTail.size());
        m_logDumpTail.clear();
    }
}

void SerialWorker::ensurePort() {
    if (m_port)
        return;

    // Created here, after this object has been moved to the serial thread.
    m_port = new QSerialPort(this);
    emit diagnostic(QStringLiteral("Created new QSerialPort instance"));

    connect(m_port, &QSerialPort::readyRead, this, [this] {
        if (!m_port)
            return;
        const QByteArray chunk = m_port->readAll();
        if (chunk.isEmpty())
            return;

        // Logging stays in the serial thread so disk I/O does not block the
        // GUI thread. The selected log view is independent from the RX view.
        writeLogChunk(chunk);
        emit dataReceived(chunk);
    });

    connect(m_port, &QSerialPort::bytesWritten,
            this, &SerialWorker::bytesWritten);

    connect(m_port, &QSerialPort::errorOccurred, this,
            [this](QSerialPort::SerialPortError error) {
        if (error == QSerialPort::NoError || !m_port)
            return;

        const QString message = m_port->errorString();
        emit diagnostic(QStringLiteral("Serial error %1: %2")
                            .arg(static_cast<int>(error))
                            .arg(message));

        if (error == QSerialPort::ResourceError ||
            error == QSerialPort::DeviceNotFoundError) {
            // Do not reuse a QSerialPort object after a fatal device removal.
            // Some USB-serial drivers leave the native handle in a bad state;
            // a fresh object on the next Connect avoids carrying that state.
            emit diagnostic(QStringLiteral("Fatal serial error: serial object will be recreated"));
            discardPort();
            emit connectionLost(QStringLiteral("port is no longer available"));
            return;
        }

        emit errorOccurred(message);
    });
}

void SerialWorker::discardPort() {
    if (!m_port)
        return;

    QSerialPort *oldPort = m_port;
    m_port = nullptr;
    oldPort->disconnect(this);
    if (oldPort->isOpen())
        oldPort->close();
    oldPort->deleteLater();
    emit diagnostic(QStringLiteral("Discarded QSerialPort instance"));
}

void SerialWorker::ensureLogFile() {
    if (!m_logFile)
        m_logFile = new QFile(this);
}

void SerialWorker::closeLogFile() {
    if (!m_logFile || !m_logFile->isOpen())
        return;

    m_logFile->flush();
    m_logFile->close();
}

void SerialWorker::openPort(const SerialSettings &settings) {
    // Start every explicit connection attempt with a fresh QSerialPort/native
    // handle. This is cheap and avoids reusing driver state left behind by a
    // previous failed or removed USB-serial device.
    if (m_port)
        discardPort();
    ensurePort();

    emit diagnostic(QStringLiteral("Open %1 @ %2")
                        .arg(settings.portName)
                        .arg(settings.baudRate));
    m_port->setPortName(settings.portName);

    const bool baudOk = m_port->setBaudRate(settings.baudRate);
    emit diagnostic(QStringLiteral("setBaudRate(%1): %2")
                        .arg(settings.baudRate)
                        .arg(baudOk ? QStringLiteral("OK") : QStringLiteral("FAILED")));
    const bool dataBitsOk = m_port->setDataBits(settings.dataBits);
    emit diagnostic(QStringLiteral("setDataBits(%1): %2")
                        .arg(static_cast<int>(settings.dataBits))
                        .arg(dataBitsOk ? QStringLiteral("OK") : QStringLiteral("FAILED")));
    const bool parityOk = m_port->setParity(settings.parity);
    emit diagnostic(QStringLiteral("setParity(%1): %2")
                        .arg(static_cast<int>(settings.parity))
                        .arg(parityOk ? QStringLiteral("OK") : QStringLiteral("FAILED")));
    const bool stopBitsOk = m_port->setStopBits(settings.stopBits);
    emit diagnostic(QStringLiteral("setStopBits(%1): %2")
                        .arg(static_cast<int>(settings.stopBits))
                        .arg(stopBitsOk ? QStringLiteral("OK") : QStringLiteral("FAILED")));
    const bool flowOk = m_port->setFlowControl(settings.flowControl);
    emit diagnostic(QStringLiteral("setFlowControl(%1): %2")
                        .arg(static_cast<int>(settings.flowControl))
                        .arg(flowOk ? QStringLiteral("OK") : QStringLiteral("FAILED")));

    if (!(baudOk && dataBitsOk && parityOk && stopBitsOk && flowOk)) {
        emit errorOccurred(QStringLiteral("Unsupported serial settings"));
        return;
    }

    if (!m_port->open(QIODevice::ReadWrite)) {
        const QString error = m_port->errorString();
        emit diagnostic(QStringLiteral("Open failed: %1").arg(error));
        emit errorOccurred(QStringLiteral("%1: %2").arg(settings.portName, error));
        return;
    }
    emit diagnostic(QStringLiteral("Open succeeded"));

    const bool dtrOk = m_port->setDataTerminalReady(settings.dataTerminalReady);
    emit diagnostic(QStringLiteral("DTR -> %1: %2")
                        .arg(settings.dataTerminalReady ? 1 : 0)
                        .arg(dtrOk ? QStringLiteral("OK") : QStringLiteral("FAILED")));
    if (settings.flowControl != QSerialPort::HardwareControl) {
        const bool rtsOk = m_port->setRequestToSend(settings.requestToSend);
        emit diagnostic(QStringLiteral("RTS -> %1: %2")
                            .arg(settings.requestToSend ? 1 : 0)
                            .arg(rtsOk ? QStringLiteral("OK") : QStringLiteral("FAILED")));
    } else {
        emit diagnostic(QStringLiteral("RTS managed by hardware flow control"));
    }

    emit opened(settings.portName);
}

void SerialWorker::closePort() {
    if (!m_port || !m_port->isOpen()) {
        emit diagnostic(QStringLiteral("Close requested while port is already closed"));
        emit closed();
        return;
    }

    emit diagnostic(QStringLiteral("Closing port"));
    m_port->close();
    emit closed();
}

void SerialWorker::writeData(const QByteArray &data) {
    if (!m_port || !m_port->isOpen()) {
        emit errorOccurred(QStringLiteral("Port is not open"));
        emit diagnostic(QStringLiteral("TX rejected: port is not open"));
        return;
    }

    emit diagnostic(QStringLiteral("TX request %1 B").arg(data.size()));
    if (m_port->write(data) < 0) {
        const QString error = m_port->errorString();
        emit diagnostic(QStringLiteral("TX failed: %1").arg(error));
        emit errorOccurred(error);
    }
}

void SerialWorker::setDtr(bool enabled) {
    if (!m_port || !m_port->isOpen()) {
        emit errorOccurred(QStringLiteral("Port is not open"));
        return;
    }
    const bool ok = m_port->setDataTerminalReady(enabled);
    emit diagnostic(QStringLiteral("DTR -> %1: %2")
                        .arg(enabled ? 1 : 0)
                        .arg(ok ? QStringLiteral("OK") : QStringLiteral("FAILED")));
    if (!ok)
        emit errorOccurred(QStringLiteral("Failed to set DTR"));
}

void SerialWorker::setRts(bool enabled) {
    if (!m_port || !m_port->isOpen()) {
        emit errorOccurred(QStringLiteral("Port is not open"));
        return;
    }
    if (m_port->flowControl() == QSerialPort::HardwareControl) {
        emit errorOccurred(QStringLiteral("RTS is controlled by hardware flow control"));
        return;
    }
    const bool ok = m_port->setRequestToSend(enabled);
    emit diagnostic(QStringLiteral("RTS -> %1: %2")
                        .arg(enabled ? 1 : 0)
                        .arg(ok ? QStringLiteral("OK") : QStringLiteral("FAILED")));
    if (!ok)
        emit errorOccurred(QStringLiteral("Failed to set RTS"));
}

void SerialWorker::startLog(const QString &path, int format, bool timestamps) {
    if (path.isEmpty())
        return;

    ensureLogFile();
    flushPendingLogData();
    closeLogFile();

    m_logPath = path;
    m_logFormat = qBound(0, format, 2);
    m_logTimestamps = timestamps;
    m_logLineStart = true;
    m_logDumpTail.clear();
    m_logDumpOffset = 0;

    m_logFile->setFileName(path);
    if (!m_logFile->open(QIODevice::WriteOnly | QIODevice::Truncate)) {
        emit errorOccurred(QStringLiteral("Log: %1")
                               .arg(m_logFile->errorString()));
        emit logStateChanged(false, path);
        return;
    }

    emit logStateChanged(true, path);
}

void SerialWorker::stopLog() {
    const QString path = m_logPath;
    flushPendingLogData();
    closeLogFile();
    emit logStateChanged(false, path);
}
