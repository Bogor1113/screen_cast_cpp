// client_gui.cpp -- client GUI (Qt): FFmpeg decode + D3D11 render embedded in Qt
// usage: client_gui.exe <teacherIP> [multicastGroup]
//        with a multicastGroup (e.g. 239.255.1.1) it receives via multicast; omitted = unicast
//        with no arguments it pops up the "connect teacher" dialog (remembers last-used IP)
#include <QApplication>
#include <QIcon>
#include <QPixmap>
#include <QImage>
#include <QMessageBox>
#include "crash_report.h"
#include "ui/client_window.h"
#include "ui/connect_dialog.h"
#include "ui/qss_dark.h"
#include "version.h"

int main(int argc, char* argv[]) {
    crash_report::install("client");   // SEH minidump + terminate/abort log, all to the current dir
    // Qt6 handles per-monitor DPI awareness itself (no SetProcessDPIAware needed here)
    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("screen_cast 学生端"));
    // Fusion style (not the Windows native style): the native style draws QComboBox arrows /
    // popup lists and QPushButton hover/pressed states with the system gray-white look, which
    // overrides the QSS. Fusion lets the stylesheet fully control these widgets.
    app.setStyle(QStringLiteral("Fusion"));
    // Pin the palette too: widgets Qt paints from the palette (menu popups, tool tips, item
    // views) otherwise follow the Windows light/dark setting -- a light-mode Windows turned the
    // video right-click menu into white-on-white.
    applyDarkPalette(app);
    app.setStyleSheet(kQssDark);

    // embedded APPICON (msi/student.ico) as the window icon
    HICON hIcon = LoadIconW(GetModuleHandleW(nullptr), L"APPICON");
    if (hIcon) {
        QImage img = QImage::fromHICON(hIcon);
        if (!img.isNull()) app.setWindowIcon(QIcon(QPixmap::fromImage(img)));
    }

    // teacher IP / multicast group: command-line args take precedence; with no args pop up the
    // "Connect teacher" dialog (defaults come from the last-saved client_config.txt)
    QString teacherIP, group;
    bool multicast = false;
    if (argc >= 2) {
        teacherIP = QString::fromUtf8(argv[1]);
        if (argc >= 3) group = QString::fromUtf8(argv[2]);
        multicast = !group.isEmpty();
    } else {
        ConnectDialog dlg;
        if (dlg.exec() != QDialog::Accepted) return 0;   // user cancelled/closed -> exit
        teacherIP = dlg.teacherIp();
        group = dlg.group();
        multicast = !group.isEmpty();   // empty multicast group in the dialog means unicast
        ConnectDialog::saveConfig(teacherIP, group);
    }
    // also save the connection info to disk when command-line args were given
    if (argc >= 2) ConnectDialog::saveConfig(teacherIP, group);

    ClientWindow w(teacherIP, group);
    if (!w.start()) {
        QMessageBox::critical(nullptr,
                              QStringLiteral("screen_cast 客户端 v") + QString::fromWCharArray(SCREENCAST_VERSION_WS),
                              QStringLiteral("收包线程启动失败(检查教师端 IP 与网络): %1").arg(teacherIP));
        return 1;
    }
w.setWindowTitle(QString::fromWCharArray(SCREENCAST_VERSION_WS));
w.showMaximized();   // connect → maximized window WITH title bar (F11/Esc to restore)
return app.exec();
}