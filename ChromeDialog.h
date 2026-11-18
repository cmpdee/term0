#pragma once

#include "FlipperChrome.h"

#include <QDialog>
#include <QList>
#include <QMargins>

// A conventional QDialog in native mode; in either alternate theme a small
// painted title bar and compositor-assisted resize handles replace OS chrome.
// The contents remain regular Qt widgets, so dialog validation, modal exec(),
// keyboard navigation and accessibility keep their usual behavior.
class ChromeDialog : public QDialog {
    Q_OBJECT
public:
    explicit ChromeDialog(QWidget *parent = nullptr);
    void setChromeTheme(ChromeTheme theme);
    ChromeTheme chromeTheme() const { return m_theme; }

protected:
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;

private:
    void placeChrome();
    void placeHandles();

    ChromeTheme m_theme = ChromeTheme::Native;
    FlipperTitleBar *m_titleBar = nullptr;
    QList<FlipperResizeHandle *> m_handles;
    QMargins m_originalMargins;
    bool m_haveOriginalMargins = false;
};
