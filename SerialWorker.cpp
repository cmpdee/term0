#include "SerialWorker.h"

#include <QFile>

SerialWorker::SerialWorker(QObject *parent) : QObject(parent) {}

SerialWorker::~SerialWorker() {
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

void SerialWorker::ensurePort() {
    if (m_port)
        return;

    // Created here, after this object has been moved to the serial thread.
    m_port = new QSerialPort(this);

    connect(m_port, &QSerialPort::readyRead, this, [this] {
        const QByteArray chunk = m_port->readAll();
        if (chunk.isEmpty())
            return;

        // Text logging deliberately mirrors term0's Text view convention:
        // preserve CR/LF/TAB and printable bytes, replace other C0 controls
        // with '.'. Logging stays in the serial thread so disk I/O does not
        // block the GUI thread.
        if (m_logFile && m_logFile->isOpen()) {
            const QByteArray textChunk = sanitizeTextBytes(chunk);
            const qint64 written = m_logFile->write(textChunk);
            if (written > 0)
                emit logBytesWritten(written);

            if (written != textChunk.size()) {
                const QString path = m_logPath;
                const QString message = QStringLiteral("Log write failed: %1")
                                            .arg(m_logFile->errorString());
                closeLogFile();
                emit logStateChanged(false, path);
                emit errorOccurred(message);
            }
        }

        emit dataReceived(chunk);
    });

    connect(m_port, &QSerialPort::bytesWritten,
            this, &SerialWorker::bytesWritten);

    connect(m_port, &QSerialPort::errorOccurred, this,
            [this](QSerialPort::SerialPortError error) {
        if (error == QSerialPort::NoError)
            return;

        const QString message = m_port->errorString();

        // A physically removed USB-UART commonly reports ResourceError.
        // DeviceNotFoundError is also fatal for the current connection.
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
    m_port->setBaudRate(settings.baudRate);
    m_port->setDataBits(settings.dataBits);
    m_port->setParity(settings.parity);
    m_port->setStopBits(settings.stopBits);
    m_port->setFlowControl(settings.flowControl);

    if (!m_port->open(QIODevice::ReadWrite)) {
        emit errorOccurred(QStringLiteral("%1: %2")
                               .arg(settings.portName, m_port->errorString()));
        return;
    }

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

void SerialWorker::startLog(const QString &path) {
    if (path.isEmpty())
        return;

    ensureLogFile();
    closeLogFile();

    m_logPath = path;
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
    closeLogFile();
    emit logStateChanged(false, path);
}
