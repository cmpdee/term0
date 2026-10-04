#include <QApplication>
#include <QIcon>
#include <QSize>
#include "MainWindow.h"

int main(int argc, char *argv[]) {
    QApplication app(argc, argv);
    QApplication::setApplicationName(QStringLiteral("term0"));
    QApplication::setOrganizationName(QStringLiteral("term0"));

    // Use dedicated small artwork instead of letting Windows downscale the
    // detailed desktop icon. This keeps the taskbar icon readable at 16-32 px.
    QIcon icon;
    icon.addFile(QStringLiteral(":/assets/term0-16.png"), QSize(16, 16));
    icon.addFile(QStringLiteral(":/assets/term0-20.png"), QSize(20, 20));
    icon.addFile(QStringLiteral(":/assets/term0-24.png"), QSize(24, 24));
    icon.addFile(QStringLiteral(":/assets/term0-32.png"), QSize(32, 32));
    icon.addFile(QStringLiteral(":/assets/term0-48.png"), QSize(48, 48));
    icon.addFile(QStringLiteral(":/assets/term0-64.png"), QSize(64, 64));
    icon.addFile(QStringLiteral(":/assets/term0-128.png"), QSize(128, 128));
    icon.addFile(QStringLiteral(":/assets/term0-256.png"), QSize(256, 256));
    QApplication::setWindowIcon(icon);

    MainWindow window;
    window.show();
    return app.exec();
}
