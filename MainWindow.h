#pragma once

#include <QElapsedTimer>
#include <QFutureWatcher>
#include <QMainWindow>
#include <QSerialPortInfo>
#include <QThread>
#include <QStringList>
#include <QTimer>

#include "SerialWorker.h"

class QComboBox;
class QLabel;
class QLineEdit;
class QPlainTextEdit;
class QPushButton;
class QCheckBox;
class QEvent;

class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

signals:
    void requestOpen(const SerialSettings &settings);
    void requestClose();
    void requestWrite(const QByteArray &data);
    void requestStartLog(const QString &path);
    void requestStopLog();

private slots:
    void scanPortsAsync();
    void applyScannedPorts();
    void toggleConnection();
    void toggleLog();
    void saveTextSnapshot();
    void sendInput();
    void clearTerminal();
    void queueIncoming(const QByteArray &data);
    void flushTerminal();
    void updateSessionStatus();
    void renderDisplayHistory();

protected:
    bool eventFilter(QObject *watched, QEvent *event) override;

private:
    void buildUi();
    void setConnectedUi(bool connected);
    SerialSettings currentSettings() const;
    static QString portLabel(const QSerialPortInfo &info);
    static QByteArray sanitizeTextBytes(const QByteArray &data);
    static bool parseHexInput(const QString &text, QByteArray &result,
                              QString &errorMessage);
    QByteArray formatHexBytes(const QByteArray &data);
    static QByteArray formatHexDumpLine(const QByteArray &data,
                                        quint64 offset,
                                        bool appendNewline);
    static QByteArray formatHexDump(const QByteArray &data,
                                    quint64 startOffset);
    void appendHexDumpChunk(const QByteArray &data);
    static QString formatElapsed(qint64 milliseconds);

    QComboBox *m_ports = nullptr;
    QComboBox *m_baud = nullptr;
    QComboBox *m_rxView = nullptr;
    QPushButton *m_connect = nullptr;
    QPushButton *m_clear = nullptr;
    QPushButton *m_log = nullptr;
    QPlainTextEdit *m_terminal = nullptr;
    QLineEdit *m_input = nullptr;
    QPushButton *m_send = nullptr;
    QCheckBox *m_txHex = nullptr;
    QCheckBox *m_cr = nullptr;
    QCheckBox *m_lf = nullptr;
    QLabel *m_status = nullptr;
    QLabel *m_sessionStats = nullptr;

    QFutureWatcher<QList<QSerialPortInfo>> m_scanWatcher;
    QTimer m_autoScanTimer;
    QTimer m_flushTimer;
    QTimer m_statusTimer;
    QElapsedTimer m_connectionElapsed;
    QByteArray m_rxBuffer;
    QByteArray m_displayHistory;
    quint64 m_displayHistoryBaseOffset = 0;
    QStringList m_lastPortKeys;
    QString m_preferredPort;
    QString m_connectedPort;
    qint32 m_connectedBaud = 0;
    quint64 m_rxBytes = 0;
    quint64 m_txBytes = 0;
    qint64 m_lastElapsedMs = 0;
    QByteArray m_dumpTail;
    quint64 m_dumpNextOffset = 0;
    int m_dumpPartialChars = 0;

    QStringList m_commandHistory;
    int m_historyIndex = 0;
    QString m_historyDraft;

    bool m_logActive = false;
    quint64 m_logBytes = 0;
    QString m_logPath;

    QThread m_serialThread;
    SerialWorker *m_serialWorker = nullptr;
    bool m_connected = false;
};
