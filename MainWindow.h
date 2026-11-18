#pragma once

#include <QFont>
#include <QList>
#include <QMainWindow>
#include <QPointer>

class QCloseEvent;
class QResizeEvent;
class QEvent;
class FlipperTabBar;
class FlipperTitleBar;
class FlipperResizeHandle;
class QStackedWidget;
class QTabBar;
class SessionWidget;

class MainWindow final : public QMainWindow {
    Q_OBJECT
public:
    explicit MainWindow(QWidget *parent = nullptr);
    ~MainWindow() override;

protected:
    void closeEvent(QCloseEvent *event) override;
    void resizeEvent(QResizeEvent *event) override;
    void changeEvent(QEvent *event) override;

private slots:
    void addSession();
    void closeCurrentSession();
    void closeSession(int index);
    void activateSession(int index);
    void toggleHiddenTheme();
    void toggleFlipperTheme();

private:
    void buildUi();
    void loadWindowPreferences();
    void saveWindowPreferences() const;
    void updateTabBarVisibility();
    void setCustomChrome(bool enabled);
    void updateResizeHandles();
    void applyHiddenTheme(bool enabled);
    void applyFlipperTheme(bool enabled);
    SessionWidget *currentSession() const;
    int nextAvailableSessionNumber() const;

    FlipperTabBar *m_tabBar = nullptr;
    FlipperTitleBar *m_titleBar = nullptr;
    QList<FlipperResizeHandle *> m_resizeHandles;
    bool m_customChrome = false;
    QStackedWidget *m_stack = nullptr;
    QFont m_standardUiFont;
    bool m_hiddenTheme = false;
    bool m_flipperTheme = false;
    QPointer<SessionWidget> m_activeSession;
};
