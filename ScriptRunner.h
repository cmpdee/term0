#pragma once

#include <QByteArray>
#include "ChromeDialog.h"
#include <QTimer>
#include <QString>
#include <QStringList>
#include <QVector>

class QLabel;
class QPlainTextEdit;
class QPushButton;

class ScriptRunnerDialog final : public ChromeDialog {
    Q_OBJECT
public:
    explicit ScriptRunnerDialog(QWidget *parent = nullptr);

    void setConnected(bool connected);
    void setInitialPath(const QString &path);
    void setSessionLabel(const QString &label);
    void feedRx(const QByteArray &data);

signals:
    void sendText(const QString &text, bool appendLineEnding);
    void sendRaw(const QByteArray &data);
    void setDtr(bool enabled);
    void setRts(bool enabled);
    void pathChanged(const QString &path);

private slots:
    void openScript();
    void saveScript();
    void runScript();
    void stopScript();
    void runNext();
    void waitTimedOut();

private:
    enum class CommandType {
        Send,
        SendLine,
        Hex,
        Sleep,
        Expect,
        Dtr,
        Rts
    };

    struct Command {
        CommandType type = CommandType::Send;
        QString text;
        QByteArray bytes;
        int value = 0;
        int timeoutMs = 0;
        int sourceLine = 0;
    };

    bool parseScript(QVector<Command> &commands, QString &error) const;
    static bool parseBlock(const QStringList &lines, int &index,
                           QVector<Command> &out, QString &error,
                           bool expectClosingBrace);
    static bool parseQuoted(const QString &text, QString &value,
                            QString &trailing, QString &error);
    static QString unescapeText(const QString &text, bool &ok, QString &error);
    static bool parseHex(const QString &text, QByteArray &data, QString &error);
    void setRunning(bool running);
    void finish(const QString &message);
    void fail(const QString &message);

    QPlainTextEdit *m_editor = nullptr;
    QLabel *m_status = nullptr;
    QPushButton *m_open = nullptr;
    QPushButton *m_save = nullptr;
    QPushButton *m_run = nullptr;
    QPushButton *m_stop = nullptr;

    QVector<Command> m_commands;
    int m_index = 0;
    bool m_running = false;
    bool m_connected = false;
    bool m_waitingForExpect = false;
    QByteArray m_expectNeedle;
    QByteArray m_rxWindow;
    QTimer m_timer;
    QString m_path;
    QString m_sessionLabel;
    void updateTitle();
};
