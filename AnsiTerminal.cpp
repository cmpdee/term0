#include "AnsiTerminal.h"

#include <QColor>
#include <QFont>
#include <QPlainTextEdit>
#include <QScrollBar>
#include <QStringDecoder>
#include <QTextCharFormat>
#include <QTextCursor>
#include <QVector>

#include <algorithm>

namespace {
constexpr int kMaxAnsiLines = 5000;
constexpr int kMaxAnsiCells = 500000;
constexpr int kMaxCursorColumn = 4096;
constexpr int kMaxCsiBytes = 128;

struct AnsiAttributes {
    QColor foreground;
    QColor background;
    bool bold = false;

    bool operator==(const AnsiAttributes &other) const {
        return foreground == other.foreground &&
               background == other.background &&
               bold == other.bold;
    }
};

struct AnsiCell {
    QChar ch = QLatin1Char(' ');
    AnsiAttributes attributes;
};

QColor ansi16Color(int index) {
    static const QColor colors[] = {
        QColor(QStringLiteral("#000000")), QColor(QStringLiteral("#aa0000")),
        QColor(QStringLiteral("#00aa00")), QColor(QStringLiteral("#aa5500")),
        QColor(QStringLiteral("#0000aa")), QColor(QStringLiteral("#aa00aa")),
        QColor(QStringLiteral("#00aaaa")), QColor(QStringLiteral("#aaaaaa")),
        QColor(QStringLiteral("#555555")), QColor(QStringLiteral("#ff5555")),
        QColor(QStringLiteral("#55ff55")), QColor(QStringLiteral("#ffff55")),
        QColor(QStringLiteral("#5555ff")), QColor(QStringLiteral("#ff55ff")),
        QColor(QStringLiteral("#55ffff")), QColor(QStringLiteral("#ffffff"))
    };
    return colors[qBound(0, index, 15)];
}

QColor ansi256Color(int index) {
    index = qBound(0, index, 255);
    if (index < 16)
        return ansi16Color(index);
    if (index >= 232) {
        const int level = 8 + (index - 232) * 10;
        return QColor(level, level, level);
    }

    const int cube = index - 16;
    const int r = cube / 36;
    const int g = (cube / 6) % 6;
    const int b = cube % 6;
    const auto component = [](int value) {
        return value == 0 ? 0 : 55 + value * 40;
    };
    return QColor(component(r), component(g), component(b));
}

QTextCharFormat textFormat(const AnsiAttributes &attributes) {
    QTextCharFormat format;
    if (attributes.foreground.isValid())
        format.setForeground(attributes.foreground);
    if (attributes.background.isValid())
        format.setBackground(attributes.background);
    if (attributes.bold)
        format.setFontWeight(QFont::Bold);
    return format;
}

QVector<int> csiParameters(QByteArray payload) {
    while (!payload.isEmpty()) {
        const char first = payload.front();
        if (first == '?' || first == '>' || first == '!' || first == '=')
            payload.remove(0, 1);
        else
            break;
    }

    payload.replace(':', ';');
    QVector<int> result;
    const QList<QByteArray> parts = payload.split(';');
    for (const QByteArray &part : parts) {
        if (part.isEmpty()) {
            result.append(0);
            continue;
        }
        bool ok = false;
        const int value = part.toInt(&ok);
        result.append(ok ? value : 0);
    }
    if (result.isEmpty())
        result.append(0);
    return result;
}

int parameterOr(const QVector<int> &params, int index, int defaultValue,
                bool zeroMeansDefault = true) {
    if (index < 0 || index >= params.size())
        return defaultValue;
    const int value = params.at(index);
    return zeroMeansDefault && value == 0 ? defaultValue : value;
}
} // namespace

struct AnsiTerminalState::Impl {
    enum class ParseState {
        Ground,
        Escape,
        Csi,
        Osc,
        OscEscape
    };

    QVector<QVector<AnsiCell>> lines{QVector<AnsiCell>{}};
    int row = 0;
    int column = 0;
    int savedRow = 0;
    int savedColumn = 0;
    AnsiAttributes attributes;
    ParseState state = ParseState::Ground;
    QByteArray csi;
    QByteArray printable;
    std::unique_ptr<QStringDecoder> decoder =
        std::make_unique<QStringDecoder>(QStringDecoder::Utf8);

    void reset() {
        lines.clear();
        lines.append(QVector<AnsiCell>{});
        row = 0;
        column = 0;
        savedRow = 0;
        savedColumn = 0;
        attributes = {};
        state = ParseState::Ground;
        csi.clear();
        printable.clear();
        decoder = std::make_unique<QStringDecoder>(QStringDecoder::Utf8);
    }

    void ensureRow(int requestedRow) {
        requestedRow = qBound(0, requestedRow, kMaxAnsiLines - 1);
        while (lines.size() <= requestedRow)
            lines.append(QVector<AnsiCell>{});
    }

    void putChar(QChar ch) {
        ensureRow(row);
        column = qBound(0, column, kMaxCursorColumn);
        QVector<AnsiCell> &line = lines[row];
        while (line.size() < column)
            line.append(AnsiCell{});

        AnsiCell cell;
        cell.ch = ch;
        cell.attributes = attributes;
        if (column < line.size())
            line[column] = cell;
        else
            line.append(cell);
        if (column < kMaxCursorColumn)
            ++column;
    }

    void flushPrintable() {
        if (printable.isEmpty())
            return;
        const QString text = (*decoder)(printable);
        printable.clear();
        for (const QChar ch : text)
            putChar(ch);
    }

    void lineFeed() {
        if (row < kMaxAnsiLines - 1)
            ++row;
        ensureRow(row);
    }

    void trimOldLines() {
        int removeCount = qMax(0, lines.size() - kMaxAnsiLines);
        qsizetype cellCount = 0;
        for (const auto &line : lines)
            cellCount += line.size();
        while (cellCount > kMaxAnsiCells && removeCount < lines.size() - 1) {
            cellCount -= lines.at(removeCount).size();
            ++removeCount;
        }
        if (removeCount <= 0)
            return;
        lines.remove(0, removeCount);
        row = qMax(0, row - removeCount);
        savedRow = qMax(0, savedRow - removeCount);
    }

    void eraseLine(int mode) {
        ensureRow(row);
        QVector<AnsiCell> &line = lines[row];
        if (mode == 2) {
            line.clear();
            return;
        }
        if (mode == 1) {
            while (line.size() <= column)
                line.append(AnsiCell{});
            const int last = qMin(column, line.size() - 1);
            for (int i = 0; i <= last; ++i)
                line[i] = AnsiCell{};
            return;
        }

        if (column < line.size())
            line.resize(column);
    }

    void eraseDisplay(int mode) {
        ensureRow(row);
        if (mode == 2 || mode == 3) {
            lines.clear();
            lines.append(QVector<AnsiCell>{});
            row = 0;
            column = 0;
            return;
        }

        if (mode == 1) {
            for (int r = 0; r < row && r < lines.size(); ++r)
                lines[r].clear();
            QVector<AnsiCell> &line = lines[row];
            while (line.size() <= column)
                line.append(AnsiCell{});
            const int last = qMin(column, line.size() - 1);
            for (int i = 0; i <= last; ++i)
                line[i] = AnsiCell{};
            return;
        }

        QVector<AnsiCell> &line = lines[row];
        if (column < line.size())
            line.resize(column);
        while (lines.size() > row + 1)
            lines.removeLast();
    }

    void applySgr(const QVector<int> &params) {
        for (int i = 0; i < params.size(); ++i) {
            const int p = params.at(i);
            if (p == 0) {
                attributes = {};
            } else if (p == 1) {
                attributes.bold = true;
            } else if (p == 22) {
                attributes.bold = false;
            } else if (p >= 30 && p <= 37) {
                attributes.foreground = ansi16Color(p - 30);
            } else if (p == 39) {
                attributes.foreground = QColor();
            } else if (p >= 40 && p <= 47) {
                attributes.background = ansi16Color(p - 40);
            } else if (p == 49) {
                attributes.background = QColor();
            } else if (p >= 90 && p <= 97) {
                attributes.foreground = ansi16Color(8 + p - 90);
            } else if (p >= 100 && p <= 107) {
                attributes.background = ansi16Color(8 + p - 100);
            } else if ((p == 38 || p == 48) && i + 1 < params.size()) {
                const bool foreground = p == 38;
                if (params.at(i + 1) == 5 && i + 2 < params.size()) {
                    const QColor color = ansi256Color(params.at(i + 2));
                    if (foreground)
                        attributes.foreground = color;
                    else
                        attributes.background = color;
                    i += 2;
                } else if (params.at(i + 1) == 2 && i + 4 < params.size()) {
                    const QColor color(qBound(0, params.at(i + 2), 255),
                                       qBound(0, params.at(i + 3), 255),
                                       qBound(0, params.at(i + 4), 255));
                    if (foreground)
                        attributes.foreground = color;
                    else
                        attributes.background = color;
                    i += 4;
                }
            }
        }
    }

    void executeCsi(char finalByte) {
        QByteArray payload = csi;
        csi.clear();
        const QVector<int> params = csiParameters(payload);

        switch (finalByte) {
        case 'A':
            row = qMax(0, row - parameterOr(params, 0, 1));
            break;
        case 'B':
            row = qMin(kMaxAnsiLines - 1, row + parameterOr(params, 0, 1));
            ensureRow(row);
            break;
        case 'C':
            column = qMin(kMaxCursorColumn, column + parameterOr(params, 0, 1));
            break;
        case 'D':
            column = qMax(0, column - parameterOr(params, 0, 1));
            break;
        case 'E':
            row = qMin(kMaxAnsiLines - 1, row + parameterOr(params, 0, 1));
            column = 0;
            ensureRow(row);
            break;
        case 'F':
            row = qMax(0, row - parameterOr(params, 0, 1));
            column = 0;
            break;
        case 'G':
            column = qBound(0, parameterOr(params, 0, 1) - 1, kMaxCursorColumn);
            break;
        case 'H':
        case 'f':
            row = qBound(0, parameterOr(params, 0, 1) - 1, kMaxAnsiLines - 1);
            column = qBound(0, parameterOr(params, 1, 1) - 1, kMaxCursorColumn);
            ensureRow(row);
            break;
        case 'd':
            row = qBound(0, parameterOr(params, 0, 1) - 1, kMaxAnsiLines - 1);
            ensureRow(row);
            break;
        case 'J':
            eraseDisplay(parameterOr(params, 0, 0, false));
            break;
        case 'K':
            eraseLine(parameterOr(params, 0, 0, false));
            break;
        case 'm':
            applySgr(params);
            break;
        case 's':
            savedRow = row;
            savedColumn = column;
            break;
        case 'u':
            row = qBound(0, savedRow, kMaxAnsiLines - 1);
            column = qBound(0, savedColumn, kMaxCursorColumn);
            ensureRow(row);
            break;
        default:
            // Private modes and unsupported commands are intentionally ignored.
            break;
        }
    }

    void feed(const QByteArray &data) {
        for (const char raw : data) {
            const auto byte = static_cast<unsigned char>(raw);
            switch (state) {
            case ParseState::Ground:
                if (byte == 0x1b) {
                    flushPrintable();
                    state = ParseState::Escape;
                } else if (byte == '\r') {
                    flushPrintable();
                    column = 0;
                } else if (byte == '\n') {
                    flushPrintable();
                    lineFeed();
                } else if (byte == '\b') {
                    flushPrintable();
                    column = qMax(0, column - 1);
                } else if (byte == '\t') {
                    flushPrintable();
                    column = qMin(kMaxCursorColumn, ((column / 8) + 1) * 8);
                } else if (byte < 0x20 || byte == 0x7f) {
                    flushPrintable();
                    // BEL and unsupported C0 controls are ignored in ANSI mode.
                } else {
                    printable.append(raw);
                }
                break;

            case ParseState::Escape:
                if (byte == '[') {
                    csi.clear();
                    state = ParseState::Csi;
                } else if (byte == ']') {
                    state = ParseState::Osc;
                } else if (byte == '7') {
                    savedRow = row;
                    savedColumn = column;
                    state = ParseState::Ground;
                } else if (byte == '8') {
                    row = savedRow;
                    column = savedColumn;
                    ensureRow(row);
                    state = ParseState::Ground;
                } else if (byte == 'c') {
                    reset();
                } else if (byte == 0x1b) {
                    state = ParseState::Escape;
                } else {
                    state = ParseState::Ground;
                }
                break;

            case ParseState::Csi:
                if (byte >= 0x40 && byte <= 0x7e) {
                    executeCsi(static_cast<char>(byte));
                    state = ParseState::Ground;
                } else if (byte == 0x1b) {
                    csi.clear();
                    state = ParseState::Escape;
                } else if (csi.size() < kMaxCsiBytes) {
                    csi.append(raw);
                } else {
                    csi.clear();
                    state = ParseState::Ground;
                }
                break;

            case ParseState::Osc:
                if (byte == 0x07) {
                    state = ParseState::Ground;
                } else if (byte == 0x1b) {
                    state = ParseState::OscEscape;
                }
                break;

            case ParseState::OscEscape:
                state = byte == '\\' ? ParseState::Ground : ParseState::Osc;
                break;
            }
        }

        if (state == ParseState::Ground)
            flushPrintable();
        trimOldLines();
    }
};

AnsiTerminalState::AnsiTerminalState() : d(std::make_unique<Impl>()) {}
AnsiTerminalState::~AnsiTerminalState() = default;

void AnsiTerminalState::reset() {
    d->reset();
}

void AnsiTerminalState::feed(const QByteArray &data) {
    d->feed(data);
}

void AnsiTerminalState::render(QPlainTextEdit *terminal, bool forceBottom) const {
    if (!terminal)
        return;

    QScrollBar *horizontal = terminal->horizontalScrollBar();
    QScrollBar *vertical = terminal->verticalScrollBar();
    const int oldHorizontal = horizontal->value();
    const int oldVertical = vertical->value();
    const bool wasAtBottom = oldVertical >= vertical->maximum();

    terminal->setUpdatesEnabled(false);
    terminal->clear();
    QTextCursor cursor(terminal->document());

    for (int row = 0; row < d->lines.size(); ++row) {
        const QVector<AnsiCell> &line = d->lines.at(row);
        int start = 0;
        while (start < line.size()) {
            const AnsiAttributes attributes = line.at(start).attributes;
            int end = start + 1;
            while (end < line.size() && line.at(end).attributes == attributes)
                ++end;

            QString text;
            text.reserve(end - start);
            for (int i = start; i < end; ++i)
                text += line.at(i).ch;
            cursor.insertText(text, textFormat(attributes));
            start = end;
        }
        if (row + 1 < d->lines.size())
            cursor.insertBlock();
    }

    terminal->setTextCursor(cursor);
    terminal->setUpdatesEnabled(true);
    terminal->viewport()->update();

    if (forceBottom) {
        horizontal->setValue(0);
        vertical->setValue(vertical->maximum());
    } else {
        horizontal->setValue(qMin(oldHorizontal, horizontal->maximum()));
        if (wasAtBottom)
            vertical->setValue(vertical->maximum());
        else
            vertical->setValue(qMin(oldVertical, vertical->maximum()));
    }
}
