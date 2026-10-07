#pragma once
// ui/teacher_window.h -- teacher GUI main window (Qt port of the Win32 teacher_gui.cpp)
#include <QMainWindow>
#include <atomic>
#include <vector>
#include <string>
#include "core/streamer.h"
#include "ui/preview.h"

class QLineEdit;
class QComboBox;
class QCheckBox;
class QPushButton;
class QLabel;
class QScrollArea;
class QVBoxLayout;
class QFrame;
class FlowLayout;
class QTimer;
class PreviewWidget;
class QSystemTrayIcon;
class QMenu;
class QAction;
class QCloseEvent;

// the whole teacher control panel: top control bar (IP / quality / toggles / start) +
// desktop preview + client list + status bar. All streaming logic lives in Streamer
// (background threads); this window only drives it via thread-safe methods and timers.
class TeacherWindow : public QMainWindow {
    Q_OBJECT
public:
    explicit TeacherWindow(const QString& initialIp = QString(), QWidget* parent = nullptr);
    ~TeacherWindow() override;

private slots:
    void onMainTick();       // 33ms: preview refresh + stream health
    void onStatsTick();      // 500ms: status bar + client list
    void onStartClicked();
    void onQualityChanged(int idx);
    void onDiffToggled(bool on);
    void onDirtyToggled(bool on);
    void onMcToggled(bool on);
    void onAdaptiveToggled(bool on);
    void onIpEdited(const QString& text);

protected:
    void moveEvent(QMoveEvent* ev) override;
    void resizeEvent(QResizeEvent* ev) override;
    void changeEvent(QEvent* ev) override;
    void closeEvent(QCloseEvent* ev) override;   // close → minimize to tray (unless really quitting)
    void showEvent(QShowEvent* ev) override;     // (re)apply the capture exclusion once really on screen

private:
    void buildUi();
    void startStreaming();
    void stopStreaming();
    void applyStartBtnStyle(bool streaming);   // inline setStyleSheet for the start/stop button
    void updateStatusIndicator(bool streaming); // header dot + text ("推流中"/"未启动")
    void updateSelfExcludeRect();
    // Keep this control panel out of the picture the students receive. Preferred mechanism is the
    // OS-level WDA_EXCLUDEFROMCAPTURE (the window stays visible to the teacher, but no capture API
    // sees it); when the OS refuses, it falls back to carving the preview pane out of the frame.
    // Returns true when the OS-level exclusion is active.
    bool setCaptureExclusion(bool on);
    void updateStats(const StreamStats& s, bool running);
    void refreshClientList();
    QWidget* makeClientCard(const QString& ip);   // one rounded card per online client
    QString detectLocalIp() const;
    static Quality qualityFromIndex(int idx);
    static const QString kDefaultGroup;
    void setupTray();          // system tray icon + right-click menu
    void showFromTray();       // restore the window from the tray
    void quitApp();            // really exit (stop streaming + quit)

    Streamer streamer_;
    PreviewBuffer preview_;
    std::vector<std::string> lastClients_;

    PreviewWidget* previewW_ = nullptr;
    QLabel* ipLabel_ = nullptr;           // read-only: this machine's real IP (students connect to it), shown in status bar
    QLineEdit* targetEdit_ = nullptr;     // stream target: client IP (unicast) or multicast group
    QComboBox* quality_ = nullptr;
    QCheckBox* diff_ = nullptr;
    QCheckBox* dirty_ = nullptr;
    QCheckBox* mc_ = nullptr;
    QCheckBox* adaptive_ = nullptr;
    QPushButton* startBtn_ = nullptr;
    QLabel* statsLabel_ = nullptr;
    QLabel* clientHeader_ = nullptr;
    QLabel* statusDot_ = nullptr;         // header status indicator dot
    QLabel* statusText_ = nullptr;        // header status text ("推流中" / "未启动")
    QScrollArea* clientScroll_ = nullptr;      // scrollable host for the client chips
    QWidget* clientListHost_ = nullptr;        // container widget inside the scroll area
    FlowLayout* clientListLayout_ = nullptr;   // flowing layout of client chips (wraps lines)
    QTimer* tickTimer_ = nullptr;
    QTimer* statsTimer_ = nullptr;

    // system tray
    QSystemTrayIcon* tray_ = nullptr;
    QMenu* trayMenu_ = nullptr;
    QAction* trayShowAct_ = nullptr;
    QAction* trayQuitAct_ = nullptr;
    bool quitting_ = false;   // true = really exiting (closeEvent accepts), false = minimize to tray
    // true while WDA_EXCLUDEFROMCAPTURE is active on this window: it is then invisible to every
    // screen-capture path (DXGI Desktop Duplication included), so students receive the desktop
    // alone -- no teacher window, and no filled rectangle where the window used to be
    bool captureExcluded_ = false;

    bool started_ = false;
    // refresh throttling
    uint64_t lastPreviewMs_ = 0, lastPreviewSeq_ = 0;
    // send-rate calc
    uint64_t g_lastBytes_ = 0, g_lastTicks_ = 0;
    uint64_t s_lastBytes_ = 0, s_lastT_ = 0;
    uint64_t s_lastFrames_ = 0;   // frames-sent baseline, for the live FPS readout in the status bar
};