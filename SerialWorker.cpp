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

    connect(m_port, &QSerialPort::readyRead, this, [this] {
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
        if (error == QSerialPort::NoError)
            return;

        const QString message = m_port->errorString();

        if (error == QSerialPort::ResourceError ||
            error == QSerialPort::DeviceNotFoundError) {
            if (m_port->isOpen())
                m_port->close();
            emit connectionLost(QStringLiteral("port is no longer available"));
            return;
        }

        emit errorOccurred(message);
    });
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
    ensurePort();

    if (m_port->isOpen())
        m_port->close();

    m_port->setPortName(settings.portName);
    const bool settingsAccepted =
        m_port->setBaudRate(settings.baudRate) &&
        m_port->setDataBits(settings.dataBits) &&
        m_port->setParity(settings.parity) &&
        m_port->setStopBits(settings.stopBits) &&
        m_port->setFlowControl(settings.flowControl);

    if (!settingsAccepted) {
        emit errorOccurred(QStringLiteral("Unsupported serial settings"));
        return;
    }

    if (!m_port->open(QIODevice::ReadWrite)) {
        emit errorOccurred(QStringLiteral("%1: %2")
                               .arg(settings.portName, m_port->errorString()));
        return;
    }

    m_port->setDataTerminalReady(settings.dataTerminalReady);
    if (settings.flowControl != QSerialPort::HardwareControl)
        m_port->setRequestToSend(settings.requestToSend);

    emit opened(settings.portName);
}

void SerialWorker::closePort() {
    if (!m_port || !m_port->isOpen()) {
        emit closed();
        return;
    }

    m_port->close();
    emit closed();
}

void SerialWorker::writeData(const QByteArray &data) {
    if (!m_port || !m_port->isOpen()) {
        emit errorOccurred(QStringLiteral("Port is not open"));
        return;
    }

    if (m_port->write(data) < 0)
        emit errorOccurred(m_port->errorString());
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
