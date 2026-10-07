// teacher_gui.cpp -- teacher GUI (Qt): preview + quality + differential + multicast mode
// usage: teacher_gui.exe [clientIP] [bitrateKbps]
#include <QApplication>
#include <QIcon>
#include <QPixmap>
#include <QImage>
#include <windows.h>
#include "crash_report.h"
#include "ui/teacher_window.h"
#include "ui/qss_dark.h"
#include "version.h"

int main(int argc, char* argv[]) {
    crash_report::install("teacher");   // SEH minidump + terminate/abort log, all to the current dir

    // single-instance guard: only one teacher process is allowed. CreateMutexW + ERROR_ALREADY_EXISTS
    // is the standard Windows pattern. The mutex handle is intentionally NOT closed here -- it stays
    // held until process exit, then the kernel releases it automatically.
    HANDLE hSingleton = CreateMutexW(nullptr, FALSE, L"screen_cast_teacher_singleton_mutex");
    if (GetLastError() == ERROR_ALREADY_EXISTS) {
        CloseHandle(hSingleton);
        MessageBoxW(nullptr, L"教师端已在运行，不能同时启动多个实例。",
                    L"screen_cast 教师端", MB_OK | MB_ICONINFORMATION);
        return 1;
    }

    QApplication app(argc, argv);
    app.setApplicationName(QStringLiteral("screen_cast 教师端"));
    // Fusion style: the Windows native style draws QComboBox arrows/popup and QPushButton
    // hover/pressed with the system gray-white look, overriding the QSS. Fusion lets the
    // stylesheet fully control these widgets (start/stop button gradient + combo box).
    app.setStyle(QStringLiteral("Fusion"));
    // Pin the palette as well: palette-painted widgets (menu popups, tool tips, item views)
    // otherwise follow the Windows light/dark setting, which broke the tray menu on light mode.
    applyDarkPalette(app);
    app.setStyleSheet(kQssDark);

    // embedded APPICON (msi/teacher.ico) as the window icon
    HICON hIcon = LoadIconW(GetModuleHandleW(nullptr), L"APPICON");
    if (hIcon) {
        QImage img = QImage::fromHICON(hIcon);
        if (!img.isNull()) app.setWindowIcon(QIcon(QPixmap::fromImage(img)));
    }

    QString initialIp;
    if (argc > 1) initialIp = QString::fromUtf8(argv[1]);

    TeacherWindow w(initialIp);
    w.setWindowTitle(QStringLiteral("screen_cast - 教师端 v") + QString::fromWCharArray(SCREENCAST_VERSION_WS));
    w.show();
    return app.exec();
}