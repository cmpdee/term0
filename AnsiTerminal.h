#pragma once

#include <QByteArray>

#include <memory>

class QPlainTextEdit;

// Lightweight ANSI/VT-style stream renderer used by the optional ANSI RX
// view. It intentionally implements only common shell-oriented control
// sequences; it is not a full terminal emulator.
class AnsiTerminalState final {
public:
    AnsiTerminalState();
    ~AnsiTerminalState();

    AnsiTerminalState(const AnsiTerminalState &) = delete;
    AnsiTerminalState &operator=(const AnsiTerminalState &) = delete;

    void reset();
    void feed(const QByteArray &data);
    void render(QPlainTextEdit *terminal, bool forceBottom = false) const;

private:
    struct Impl;
    std::unique_ptr<Impl> d;
};
