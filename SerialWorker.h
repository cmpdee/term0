#pragma once

#include <QObject>
#include <QSerialPort>

class QFile;

struct SerialSettings {
    QString portName;
    qint32 baudRate = QSerialPort::Baud115200;
    QSerialPort::DataBits dataBits = QSerialPort::Data8;
    QSerialPort::Parity parity = QSerialPort::NoParity;
    QSerialPort::StopBits stopBits = QSerialPort::OneStop;
    QSerialPort::FlowControl flowControl = QSerialPort::NoFlowControl;
    bool dataTerminalReady = true;
    bool requestToSend = false;
};
Q_DECLARE_METATYPE(SerialSettings)

class SerialWorker final : public QObject {
    Q_OBJECT
public:
    explicit SerialWorker(QObject *parent = nullptr);
    ~SerialWorker() override;

public slots:
    void openPort(const SerialSettings &settings);
    void closePort();
    void writeData(const QByteArray &data);
    void startLog(const QString &path, int format, bool timestamps);
    void stopLog();

signals:
    void opened(const QString &portName);
    void closed();
    void connectionLost(const QString &message);
    void errorOccurred(const QString &message);
    void dataReceived(const QByteArray &data);
    void bytesWritten(qint64 count);
    void logStateChanged(bool active, const QString &path);
    void logBytesWritten(qint64 count);

private:
    void ensurePort();
    void ensureLogFile();
    void closeLogFile();
    static QByteArray sanitizeTextBytes(const QByteArray &data);
    static QByteArray formatHexDumpLine(const QByteArray &data, quint64 offset);
    QByteArray timestampPrefix() const;
    void writeLogChunk(const QByteArray &chunk);
    bool writeLogData(const QByteArray &data);
    void flushPendingLogData();

    QSerialPort *m_port = nullptr;
    QFile *m_logFile = nullptr;
    QString m_logPath;
    int m_logFormat = 0;
    bool m_logTimestamps = false;
    bool m_logLineStart = true;
    QByteArray m_logDumpTail;
    quint64 m_logDumpOffset = 0;
};
