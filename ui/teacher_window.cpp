// ui/teacher_window.cpp -- teacher GUI main window (Qt port of teacher_gui.cpp Win32 version)
#include "ui/teacher_window.h"
#include "ui/preview_widget.h"
#include "ui/flow_layout.h"
#include "ui/qss_dark.h"
#include <QLineEdit>
#include <QComboBox>
#include <QCheckBox>
#include <QPushButton>
#include <QLabel>
#include <QScrollArea>
#include <QFrame>
#include <QHBoxLayout>
#include <QVBoxLayout>
#include <QTimer>
#include <QMessageBox>
#include <QStatusBar>
#include <QMoveEvent>
#include <QResizeEvent>
#include <QNetworkInterface>
#include <QHostAddress>
#include <QStyle>
#include <QAbstractSocket>
#include <QUdpSocket>
#include <QSystemTrayIcon>
#include <QMenu>
#include <QAction>
#include <QCloseEvent>
#include <QApplication>
#include <QScreen>
#include <QGuiApplication>
#include <QShowEvent>
#include <algorithm>

const QString TeacherWindow::kDefaultGroup = QStringLiteral("239.255.1.1");

// Panel theme: one cohesive dark surface for every widget that used to fall through to the Fusion
// light palette (window background, the three cards, the target edit, the checkboxes, the status
// bar) while the already-inline-styled widgets (quality combo, start button, client chips, IP
// label) keep winning over these rules via their own setStyleSheet. Tokens match the client:
// deep navy surfaces, #5B8DEF accent, #E8EDF7 / #8B95A8 text tiers.
static const char* kTeacherPanelQss =
    "#centralRoot{background:#0B0F17;}"
    "#topBar,#previewFrame,#clientCard{background:#121826;border:1px solid #232D42;border-radius:12px;}"
    "#headerBar{background:transparent;}"
    "#fieldLabel{color:#8B95A8;font-size:13px;background:transparent;}"
    "#clientHeader{color:#8B95A8;font-size:12px;font-weight:600;letter-spacing:1px;background:transparent;}"
    "QLineEdit{background:#0C111C;color:#E8EDF7;border:1px solid #27324A;border-radius:8px;"
    "padding:7px 12px;font-size:13px;selection-background-color:#2B4A6F;}"
    "QLineEdit:hover{border:1px solid #344363;}"
    "QLineEdit:focus{border:1px solid #5B8DEF;}"
    "QCheckBox{color:#C9D2E3;font-size:13px;spacing:7px;background:transparent;}"
    "QCheckBox:hover{color:#E8EDF7;}"
    "QCheckBox::indicator{width:18px;height:18px;border:1px solid #34405C;border-radius:5px;"
    "background:#0C111C;}"
    "QCheckBox::indicator:hover{border:1px solid #5B8DEF;}"
    "QCheckBox::indicator:checked{background:#5B8DEF;border:1px solid #5B8DEF;}"
    "QScrollArea{background:transparent;border:none;}"
    "#clientScroll>QWidget>QWidget{background:transparent;}"
    "#clientListHost{background:transparent;}"
    "QScrollBar:vertical{background:transparent;width:8px;margin:2px;}"
    "QScrollBar::handle:vertical{background:#2E3A55;border-radius:4px;min-height:24px;}"
    "QScrollBar::handle:vertical:hover{background:#3D4C6E;}"
    "QScrollBar::add-line:vertical,QScrollBar::sub-line:vertical{height:0;}"
    "QScrollBar::add-page:vertical,QScrollBar::sub-page:vertical{background:transparent;}";

Quality TeacherWindow::qualityFromIndex(int idx) {
    // Explicit table, index-aligned with the combo items added in buildUi(). Every stream size the
    // encoder already implements is now reachable from the UI: previously only 1080p and ORIGINAL were,
    // so a class full of weak student PCs (or a WiFi-reached room) had no way to drop a step short of
    // editing code. One click now halves the client's decode cost if it needs to.
    switch (idx) {
    case 1:  return Quality::SD720P;      // 1280x720
    case 2:  return Quality::FLUENT;      // 960x540
    case 3:  return Quality::ORIGINAL;    // source resolution
    default: return Quality::HD1080P;     // index 0 = the default, chosen FOR the classroom hardware
    }
}

TeacherWindow::TeacherWindow(const QString& initialIp, QWidget* parent) : QMainWindow(parent) {
    buildUi();
    // Fixed window: the teacher panel is a control surface, not a resize target -- a stable layout
    // keeps the preview/exclusion region deterministic. But never larger than the screen's work
    // area: on a small or high-DPI display an over-sized fixed window pushes its bottom controls
    // (start/stop, client list) off the screen, leaving the teacher unable to reach them and with
    // no way to shrink the window either.
    QSize panel(820, 520);
    if (const QScreen* sc = QGuiApplication::primaryScreen()) {
        const QRect wa = sc->availableGeometry();
        panel.setWidth(qMin(panel.width(), qMax(360, wa.width() - 40)));
        panel.setHeight(qMin(panel.height(), qMax(280, wa.height() - 40)));
    }
    setFixedSize(panel);

    // command-line IP (unicast mode) overrides the multicast default target
    if (!initialIp.isEmpty()) {
        targetEdit_->setText(initialIp);
        mc_->setChecked(false);   // explicit unicast target -> single-client mode
    }

    // register the preview callback: capture thread scale-copies into the preview buffer
    streamer_.setPreviewCallback([this](const uint8_t* bgra, int w, int h, int stride,
                                        const RECT* dirty, int n) {
        Preview_Update(preview_, bgra, w, h, stride, dirty, n);
    });

    // timers: preview refresh + stream health at 33ms; status bar throttled to 500ms
    tickTimer_ = new QTimer(this);
    tickTimer_->setInterval(33);
    connect(tickTimer_, &QTimer::timeout, this, &TeacherWindow::onMainTick);
    tickTimer_->start();

    statsTimer_ = new QTimer(this);
    statsTimer_->setInterval(500);
    connect(statsTimer_, &QTimer::timeout, this, &TeacherWindow::onStatsTick);
    statsTimer_->start();

    startStreaming();   // start streaming immediately on open
    setupTray();        // system tray icon + right-click menu
}

TeacherWindow::~TeacherWindow() {
    if (started_) stopStreaming();
}

// ---------------- system tray ----------------
void TeacherWindow::setupTray() {
    if (!QSystemTrayIcon::isSystemTrayAvailable()) return;   // no tray (rare headless env): just stay as a normal window

    QIcon icon = QApplication::windowIcon();                 // the embedded APPICON set in main()
    if (icon.isNull()) icon = style()->standardIcon(QStyle::SP_ComputerIcon);
    tray_ = new QSystemTrayIcon(icon, this);
    tray_->setToolTip(QStringLiteral("screen_cast 教师端"));

    trayMenu_ = new QMenu(this);
    // same Qt6/Fusion escape hatch as the client's video context menu: without a widget-level
    // stylesheet the tray menu background follows the system palette (white under a light
    // Windows theme) while its labels stay near-white.
    trayMenu_->setStyleSheet(kPopupMenuQss);
    trayShowAct_ = trayMenu_->addAction(QStringLiteral("显示主界面"));
    trayMenu_->addSeparator();
    trayQuitAct_ = trayMenu_->addAction(QStringLiteral("退出程序"));
    tray_->setContextMenu(trayMenu_);

    connect(trayShowAct_, &QAction::triggered, this, &TeacherWindow::showFromTray);
    connect(trayQuitAct_, &QAction::triggered, this, &TeacherWindow::quitApp);
    // double-click (or single click on some shells) restores the window
    connect(tray_, &QSystemTrayIcon::activated, this, [this](QSystemTrayIcon::ActivationReason r) {
        if (r == QSystemTrayIcon::DoubleClick || r == QSystemTrayIcon::Trigger)
            showFromTray();
    });

    tray_->show();
}

void TeacherWindow::showFromTray() {
    showNormal();
    raise();
    activateWindow();
    if (started_) updateSelfExcludeRect();   // re-arm the preview exclusion after restore
}

// Streaming starts from the constructor, i.e. before the window is mapped; SetWindowDisplayAffinity
// against a window that has never been shown can be rejected. Re-apply once it is really on screen
// (idempotent -- a successful re-apply just overwrites the same affinity).
void TeacherWindow::showEvent(QShowEvent* ev) {
    QMainWindow::showEvent(ev);
    if (started_) setCaptureExclusion(true);
}

void TeacherWindow::quitApp() {
    quitting_ = true;
    if (started_) stopStreaming();
    if (tray_) { tray_->hide(); tray_ = nullptr; }
    QApplication::quit();
}

// close (X button) → minimize to tray; real exit only via the tray "退出程序" action
void TeacherWindow::closeEvent(QCloseEvent* ev) {
    if (quitting_) { ev->accept(); return; }
    ev->ignore();
    hide();
    // window is now hidden: no preview region to exclude (otherwise students see a stale dark panel)
    streamer_.setSelfExcludeRect(RECT{ 0, 0, 0, 0 }, false);
    if (tray_ && tray_->isVisible()) {
        tray_->showMessage(QStringLiteral("screen_cast 教师端"),
                           QStringLiteral("已最小化到托盘，推流继续运行"),
                           QSystemTrayIcon::Information, 2000);
    }
}

void TeacherWindow::buildUi() {
    auto* central = new QWidget(this);
    central->setObjectName("centralRoot");
    central->setStyleSheet(kTeacherPanelQss);
    setCentralWidget(central);
    auto* root = new QVBoxLayout(central);
    root->setContentsMargins(16, 14, 16, 10);
    root->setSpacing(12);

    // ---------- header: live status (left) + local IP (right, prominent) ----------
    auto* headerBar = new QWidget(central);
    headerBar->setObjectName("headerBar");
    auto* hb = new QHBoxLayout(headerBar);
    hb->setContentsMargins(4, 0, 4, 0);
    hb->setSpacing(10);
    statusDot_ = new QLabel(headerBar);
    statusDot_->setObjectName("statusDot");
    statusDot_->setFixedSize(10, 10);
    hb->addWidget(statusDot_);
    statusText_ = new QLabel(QStringLiteral("未启动"), headerBar);
    statusText_->setObjectName("statusText");
    hb->addWidget(statusText_);
    hb->addStretch(1);
    // the address students connect to — prominent, so the teacher can copy it at a glance
    ipLabel_ = new QLabel(QStringLiteral("本机 IP  %1").arg(detectLocalIp()), headerBar);
    ipLabel_->setObjectName("ipLabel");
    ipLabel_->setStyleSheet("QLabel{color:#5B8DEF;font-size:13px;font-weight:600;background:transparent;}");
    hb->addWidget(ipLabel_);
    root->addWidget(headerBar);

    // ---------- top control bar ----------
    auto* topBar = new QWidget(central);
    topBar->setObjectName("topBar");
    auto* top = new QVBoxLayout(topBar);
    top->setContentsMargins(14, 12, 14, 12);
    top->setSpacing(10);

    // row 1: stream target + quality + start (the high-frequency controls only)
    auto* row1 = new QHBoxLayout();
    row1->setSpacing(10);

    auto* targetLabel = new QLabel(QStringLiteral("推流目标"), topBar);
    targetLabel->setObjectName("fieldLabel");
    row1->addWidget(targetLabel);
    targetEdit_ = new QLineEdit(topBar);
    targetEdit_->setPlaceholderText(QStringLiteral("客户端 IP 或组播组地址"));
    targetEdit_->setMinimumWidth(160);
    connect(targetEdit_, &QLineEdit::textEdited, this, &TeacherWindow::onIpEdited);
    row1->addWidget(targetEdit_);

    auto* qualityLabel = new QLabel(QStringLiteral("画质"), topBar);
    qualityLabel->setObjectName("fieldLabel");
    row1->addWidget(qualityLabel);
    quality_ = new QComboBox(topBar);
    quality_->setAttribute(Qt::WA_StyledBackground, true);   // let QSS background win over the native panel
    // inline stylesheet (same reason as the button: global QSS doesn't paint the combo box on Qt6/Fusion)
    quality_->setStyleSheet(
        "QComboBox{background-color:#0C111C;color:#E8EDF7;border:1px solid #27324A;"
        "border-radius:8px;padding:7px 12px;min-width:110px;}"
        "QComboBox:hover{border:1px solid #344363;}"
        "QComboBox:focus{border:1px solid #5B8DEF;}"
        "QComboBox::drop-down{border:none;width:28px;}"
        "QComboBox::down-arrow{image:none;border-left:5px solid transparent;"
        "border-right:5px solid transparent;border-top:6px solid #8B95A8;margin-right:10px;}"
        "QComboBox QAbstractItemView{background-color:#161C2A;border:1px solid #2E3750;"
        "border-radius:8px;selection-background-color:#1D2A44;selection-color:#FFFFFF;"
        "color:#C6CCE0;outline:0;padding:4px;}"
        "QComboBox QAbstractItemView::item{padding:7px 12px;min-height:24px;color:#C6CCE0;}"
        "QComboBox QAbstractItemView::item:selected{background-color:#2B4A6F;color:#FFFFFF;}");
    // Index order MUST match qualityFromIndex(). Index 0 is the default: a 1080p stream is the sharpest
    // choice for the classroom's 1366x768 / 60Hz student PCs (against their ~1366x738 client area it
    // scales to ~1312x738, whereas the teacher's full 2560x1600 source would scale to a SMALLER
    // 1181x738 -- softer, not sharper -- while doubling the student's decode cost).
    quality_->addItem(QStringLiteral("高清 1080p(推荐·适配学生机)"));
    quality_->addItem(QStringLiteral("高清 720p(学生机偏弱时选此项)"));
    quality_->addItem(QStringLiteral("流畅 540p(低配学生机/无线网络)"));
    quality_->addItem(QStringLiteral("原始分辨率(大屏展示/录制)"));
    connect(quality_, &QComboBox::currentIndexChanged, this, &TeacherWindow::onQualityChanged);
    row1->addWidget(quality_);

    row1->addStretch(1);

    startBtn_ = new QPushButton(QStringLiteral("开始推流"), topBar);
    startBtn_->setObjectName("startBtn");
    startBtn_->setAttribute(Qt::WA_StyledBackground, true);
    startBtn_->setMinimumWidth(120);
    startBtn_->setMinimumHeight(36);
    applyStartBtnStyle(false);
    connect(startBtn_, &QPushButton::clicked, this, &TeacherWindow::onStartClicked);
    row1->addWidget(startBtn_);
    top->addLayout(row1);

    // row 2: feature toggles (grouped)
    auto* row2 = new QHBoxLayout();
    row2->setSpacing(12);

    diff_ = new QCheckBox(QStringLiteral("差分投屏"), topBar);
    diff_->setChecked(true);
    connect(diff_, &QCheckBox::toggled, this, &TeacherWindow::onDiffToggled);
    row2->addWidget(diff_);

    dirty_ = new QCheckBox(QStringLiteral("变化区域"), topBar);
    connect(dirty_, &QCheckBox::toggled, this, &TeacherWindow::onDirtyToggled);
    row2->addWidget(dirty_);

    mc_ = new QCheckBox(QStringLiteral("组播模式"), topBar);
    mc_->setChecked(true);
    connect(mc_, &QCheckBox::toggled, this, &TeacherWindow::onMcToggled);
    row2->addWidget(mc_);

    adaptive_ = new QCheckBox(QStringLiteral("智能码率"), topBar);
    connect(adaptive_, &QCheckBox::toggled, this, &TeacherWindow::onAdaptiveToggled);
    row2->addWidget(adaptive_);

    row2->addStretch(1);
    top->addLayout(row2);

    root->addWidget(topBar);

    // ---------- body: preview (large) + client sidebar ----------
    auto* body = new QHBoxLayout();
    body->setSpacing(12);

    auto* previewFrame = new QWidget(central);
    previewFrame->setObjectName("previewFrame");
    auto* pf = new QVBoxLayout(previewFrame);
    pf->setContentsMargins(8, 8, 8, 8);
    previewW_ = new PreviewWidget(previewFrame);
    previewW_->setBuffer(&preview_);
    pf->addWidget(previewW_);
    body->addWidget(previewFrame, 1);   // balanced share with the sidebar (not too wide)

    auto* clientCard = new QWidget(central);
    clientCard->setObjectName("clientCard");
    clientCard->setFixedWidth(210);   // narrow sidebar
    auto* cc = new QVBoxLayout(clientCard);
    cc->setContentsMargins(12, 10, 12, 10);
    cc->setSpacing(8);
    clientHeader_ = new QLabel(QStringLiteral("在线客户端 (0)"), clientCard);
    clientHeader_->setObjectName("clientHeader");
    cc->addWidget(clientHeader_);

    // scrollable chip flow: each client is a pill tag, wrapping lines
    clientScroll_ = new QScrollArea(clientCard);
    clientScroll_->setObjectName("clientScroll");
    clientScroll_->setWidgetResizable(true);
    clientScroll_->setFrameShape(QFrame::NoFrame);
    clientListHost_ = new QWidget();
    clientListHost_->setObjectName("clientListHost");
    clientListLayout_ = new FlowLayout(clientListHost_, 0, 8, 8);   // margin 0, hSpacing 8, vSpacing 8
    clientScroll_->setWidget(clientListHost_);
    cc->addWidget(clientScroll_, 1);
    body->addWidget(clientCard);

    root->addLayout(body, 1);

    // ---------- status bar: stats ----------
    statsLabel_ = new QLabel(QStringLiteral("未启动"), this);
    statsLabel_->setStyleSheet("QLabel{color:#8B95A8;font-size:12px;background:transparent;}");
    // the status bar is a child of QMainWindow, NOT of centralRoot: the panel QSS above does not
    // reach it, so it carries its own rules (flat dark strip, no item separators)
    statusBar()->setStyleSheet(
        "QStatusBar{background:#0B0F17;color:#8B95A8;border-top:1px solid #1A2233;}"
        "QStatusBar::item{border:none;}");
    statusBar()->addWidget(statsLabel_, 1);
}

// ---------------- streaming control ----------------

void TeacherWindow::startStreaming() {
    // guard against fast double-click/re-entry
    static std::atomic<bool> s_busy{ false };
    if (s_busy.exchange(true)) return;
    if (started_) { s_busy.store(false); return; }

    streamer_.setAdaptiveBitrate(adaptive_->isChecked());
    bool mc = mc_->isChecked();
    // the stream target: multicast group address in multicast mode, else the client IP (unicast)
    QString ip = targetEdit_->text().trimmed();
    if (ip.isEmpty()) {
        ip = mc ? kDefaultGroup : detectLocalIp();
        targetEdit_->setText(ip);   // echo back the auto-detected result
    }
    streamer_.setQuality(qualityFromIndex(quality_->currentIndex()));
    streamer_.setDifferentialEnabled(diff_->isChecked());
    preview_.showDirty.store(dirty_->isChecked());
    // keep this panel out of the picture the students receive (OS-level exclusion, or the
    // preview-pane carve-out as a fallback) -- see setCaptureExclusion()
    setCaptureExclusion(true);
    if (streamer_.start(ip.toUtf8().constData(), 5004, 5005, detectLocalIp().toStdString())) {
        started_ = true;
        startBtn_->setText(QStringLiteral("停止推流"));
        startBtn_->setProperty("starting", true);
        applyStartBtnStyle(true);   // red inline stylesheet
        updateStatusIndicator(true); // green dot + "推流中"
        startBtn_->style()->unpolish(startBtn_);
        startBtn_->style()->polish(startBtn_);
        previewW_->setStreaming(true);
        updateStats(streamer_.snapshot(), true);
        s_busy.store(false);
    } else {
        setCaptureExclusion(false);   // revert on start failure
        QMessageBox::warning(this, QStringLiteral("screen_cast"),
                             QStringLiteral("推流启动失败(检查 IP 与网络)"));
        s_busy.store(false);
    }
}

void TeacherWindow::stopStreaming() {
    setCaptureExclusion(false);
    streamer_.stop();
    started_ = false;
    startBtn_->setText(QStringLiteral("开始推流"));
    startBtn_->setProperty("starting", false);
    applyStartBtnStyle(false);   // blue inline stylesheet (ready to start)
    updateStatusIndicator(false); // gray dot + "未启动"
    startBtn_->style()->unpolish(startBtn_);
    startBtn_->style()->polish(startBtn_);
    previewW_->setStreaming(false);
    updateStats(streamer_.snapshot(), false);
}

// inline setStyleSheet: Qt6/Fusion doesn't paint the button background from the global app
// stylesheet, so we paint it directly on the widget (verified with the green test button).
void TeacherWindow::applyStartBtnStyle(bool streaming) {
    if (!startBtn_) return;
    if (streaming) {
        // red gradient (visually flat red on Fusion; we use background-color for solid colour)
        startBtn_->setStyleSheet(
            "QPushButton{background-color:#E5534B;color:#FFF;border:1px solid #C94138;"
            "border-radius:8px;padding:7px 26px;font-weight:600;font-size:13px;}"
            "QPushButton:hover{background-color:#F26B60;}"
            "QPushButton:pressed{background-color:#C94138;}");
    } else {
        // blue (ready to start)
        startBtn_->setStyleSheet(
            "QPushButton{background-color:#4C8DFF;color:#FFF;border:1px solid #5B8DEF;"
            "border-radius:8px;padding:7px 26px;font-weight:600;font-size:13px;}"
            "QPushButton:hover{background-color:#5B9DFF;}"
            "QPushButton:pressed{background-color:#3D74D8;}");
    }
}

// header status indicator: green dot + "推流中" while streaming, gray dot + "未启动" otherwise
void TeacherWindow::updateStatusIndicator(bool streaming) {
    if (!statusDot_ || !statusText_) return;
    if (streaming) {
        statusDot_->setStyleSheet("QLabel{background-color:#3AB568;border-radius:5px;}");
        statusText_->setText(QStringLiteral("推流中"));
        statusText_->setStyleSheet("QLabel{color:#3AB568;font-size:13px;font-weight:600;}");
    } else {
        statusDot_->setStyleSheet("QLabel{background-color:#5C6679;border-radius:5px;}");
        statusText_->setText(QStringLiteral("未启动"));
        statusText_->setStyleSheet("QLabel{color:#8B95A8;font-size:13px;}");
    }
}

// the preview pane's on-screen rect (physical pixels, DXGI capture space): Qt gives logical
// coords, multiply by the device pixel ratio so the exclusion region matches the capture
void TeacherWindow::updateSelfExcludeRect() {
    if (!started_) return;
    if (captureExcluded_) return;   // whole window already invisible to the capture -- nothing to carve out
    if (!previewW_) return;
    // minimized windows are off the captured desktop: keep the exclusion cleared. This guard also
    // protects against the resize/move events Qt emits while minimizing, which would otherwise
    // re-arm a stale exclusion rect over the last on-screen position.
    if (isMinimized()) {
        streamer_.setSelfExcludeRect(RECT{ 0, 0, 0, 0 }, false);
        return;
    }
    QPoint tl = previewW_->mapToGlobal(QPoint(0, 0));
    QPoint br = previewW_->mapToGlobal(QPoint(previewW_->width(), previewW_->height()));
    qreal dpr = previewW_->devicePixelRatioF();
    RECT rc{ (LONG)(tl.x() * dpr), (LONG)(tl.y() * dpr),
             (LONG)(br.x() * dpr), (LONG)(br.y() * dpr) };
    streamer_.setSelfExcludeRect(rc, true);
}

// ---------------- capture exclusion ----------------
// Students must see the teacher's desktop as it is: not this control panel sitting in the middle of
// it, and not a filled rectangle where the panel used to be. Two mechanisms, best first:
//
//  1. WDA_EXCLUDEFROMCAPTURE (Windows 10 2004+). The window is drawn normally on the teacher's own
//     monitor but is excluded from every capture path -- DXGI Desktop Duplication included -- so the
//     students receive a completely clean desktop. NOTE: this must be called by the thread that OWNS
//     the window; an external process calling SetWindowDisplayAffinity on our HWND just gets FALSE.
//  2. Fallback (older Windows, or if the call is rejected): carve the preview pane out of the
//     captured frame and fill it with the panel's own background colour. That kills the recursive
//     picture-in-picture, but the rest of the panel still shows up in the picture.
bool TeacherWindow::setCaptureExclusion(bool on) {
#ifndef WDA_NONE
#define WDA_NONE 0x00000000
#endif
#ifndef WDA_EXCLUDEFROMCAPTURE
#define WDA_EXCLUDEFROMCAPTURE 0x00000011
#endif

    // Escape hatch: a WDA-excluded window is invisible to EVERY capture path -- including third
    // party screen recorders / OBS on the teacher's own machine, and our own screenshots.
    // SCREENCAST_NO_WDA=1 keeps the panel visible to capture tools at the price of the recursive
    // preview (the carve-out fallback below is skipped too). Default: off, exclusion on.
    static const bool noWda = [] {
        char b[8] = { 0 };
        DWORD n = GetEnvironmentVariableA("SCREENCAST_NO_WDA", b, sizeof(b));
        return n > 0 && b[0] == '1';
    }();
    if (on && noWda) return false;

    HWND hwnd = reinterpret_cast<HWND>(winId());   // realises the native window
    if (!hwnd) return false;

    if (!on) {
        SetWindowDisplayAffinity(hwnd, WDA_NONE);
        captureExcluded_ = false;
        streamer_.setSelfExcludeRect(RECT{ 0, 0, 0, 0 }, false);
        return false;
    }

    if (SetWindowDisplayAffinity(hwnd, WDA_EXCLUDEFROMCAPTURE)) {
        captureExcluded_ = true;
        // the window never reaches the capture, so there is no hole to fill -- leaving the carve-out
        // armed would paint a filled rectangle over the desktop for nothing
        streamer_.setSelfExcludeRect(RECT{ 0, 0, 0, 0 }, false);
        fprintf(stderr, "[teacher] 本窗口已对屏幕捕获隐藏(WDA_EXCLUDEFROMCAPTURE): 学生端将只看到桌面\n");
        return true;
    }

    captureExcluded_ = false;
    updateSelfExcludeRect();   // fallback: remove at least the recursive preview from the picture
    fprintf(stderr, "[teacher] 本机不支持隐藏窗口(WDA), 回退为遮盖预览区\n");
    return false;
}

void TeacherWindow::moveEvent(QMoveEvent* ev) {
    QMainWindow::moveEvent(ev);
    updateSelfExcludeRect();   // exclusion follows the window position
}

void TeacherWindow::resizeEvent(QResizeEvent* ev) {
    QMainWindow::resizeEvent(ev);
    updateSelfExcludeRect();   // exclusion follows the window size
}

void TeacherWindow::changeEvent(QEvent* ev) {
    QMainWindow::changeEvent(ev);
    if (ev->type() == QEvent::WindowStateChange) {
        // when the teacher window is minimized (or restored), its preview pane no longer occupies
        // the captured desktop region. Leaving the exclusion rect armed paints a stale dark panel
        // over the student's picture; clear it on minimize, re-arm it on restore.
        if (isMinimized()) {
            streamer_.setSelfExcludeRect(RECT{ 0, 0, 0, 0 }, false);
        } else if (started_) {
            updateSelfExcludeRect();
        }
    }
}

void TeacherWindow::onStartClicked() {
    started_ ? stopStreaming() : startStreaming();
}

void TeacherWindow::onQualityChanged(int idx) {
    if (started_) {
        streamer_.setQuality(qualityFromIndex(idx));
        statsLabel_->setText(QStringLiteral("清晰度已切换, 等待生效..."));
    }
}

void TeacherWindow::onDiffToggled(bool on) {
    streamer_.setDifferentialEnabled(on);
}

void TeacherWindow::onDirtyToggled(bool on) {
    preview_.showDirty.store(on);
}

void TeacherWindow::onMcToggled(bool on) {
    if (on) {
        // multicast: fill in the group address in the TARGET field when it's empty
        // (the read-only 教师IP field always shows this machine's real IP)
        if (targetEdit_->text().trimmed().isEmpty())
            targetEdit_->setText(kDefaultGroup);
    }
    statsLabel_->setText(QString());
}

void TeacherWindow::onAdaptiveToggled(bool on) {
    if (started_) {
        // mid-stream toggle: capture thread detects the change and rebuilds the encoder
        streamer_.setAdaptiveBitrate(on);
        statsLabel_->setText(QStringLiteral("智能码率已切换, 等待生效..."));
    }
}

void TeacherWindow::onIpEdited(const QString&) {
    // nothing immediate; IP is read at start
}

// ---------------- timers ----------------

void TeacherWindow::onMainTick() {
    // if the stream thread exits unexpectedly (capture/encode init failure), auto-reset the UI
    if (started_ && !streamer_.isRunning()) stopStreaming();

    // preview: redraw only when there's a new frame and >= 66ms (~15fps) since the last draw
    uint64_t now = (uint64_t)GetTickCount64();
    uint64_t seq = preview_.seq.load();
    if (seq != lastPreviewSeq_ && now - lastPreviewMs_ >= 66) {
        lastPreviewSeq_ = seq;
        lastPreviewMs_ = now;
        previewW_->update();
    }
}

void TeacherWindow::onStatsTick() {
    StreamStats s = streamer_.snapshot();
    updateStats(s, started_);
    refreshClientList();
}

void TeacherWindow::updateStats(const StreamStats& s, bool running) {
    if (running) {
        uint64_t now = (uint64_t)GetTickCount64();
        double mbps = 0.0;
        if (g_lastTicks_) {
            uint64_t dt = now - g_lastTicks_;
            if (dt > 0) mbps = (double)(s.bytesSent - g_lastBytes_) * 8.0 / dt / 1000.0;
        }
        g_lastBytes_ = s.bytesSent; g_lastTicks_ = now;
        double kbpsSend = 0.0;
        double fpsSent = 0.0;
        if (s_lastT_) {
            uint64_t dt = now - s_lastT_;
            if (dt >= 250) {
                kbpsSend = (double)(s.bytesSent - s_lastBytes_) / (double)dt * 1000.0 / 1024.0;
                // live encode/send frame rate: the single most useful number when the teacher is
                // judging whether the画面 is smooth ("画质高清 fps高 不卡顿流畅")
                fpsSent = (double)(s.framesSent - s_lastFrames_) * 1000.0 / (double)dt;
            }
        }
        s_lastBytes_ = s.bytesSent; s_lastT_ = now; s_lastFrames_ = s.framesSent;
        int targetKbps = streamer_.currentBitrateKbps();
        QString txt = QStringLiteral("采集 %1 | 发送 %2 (%3 FPS) | 差分跳过 %4   码率 %5 Mbps | 发送 %6 KB/s | 目标 %7 kbps | 丢帧 %8 | %9 包 | %10")
            .arg(s.framesCaptured).arg(s.framesSent)
            .arg(fpsSent, 0, 'f', 0)
            .arg(s.framesSkipped)
            .arg(mbps, 0, 'f', 2).arg(kbpsSend, 0, 'f', 0).arg(targetKbps)
            .arg(s.sendDropFrames).arg(s.packetsSent)
            .arg(s.gdiMode ? QStringLiteral("GDI捕获") : QStringLiteral("DXGI捕获"));
        statsLabel_->setText(txt);
    } else {
        g_lastTicks_ = 0;
        s_lastT_ = 0; s_lastFrames_ = 0;
        QString txt;
        if (mc_->isChecked())
            txt = QStringLiteral("未启动 - 组播模式: 向组 %1 推流(支持 40 客户端, 组地址可改)").arg(targetEdit_->text());
        else
            txt = QStringLiteral("未启动 - 输入客户端 IP 后点击 开始推流");
        statsLabel_->setText(txt);
    }
}

void TeacherWindow::refreshClientList() {
    std::vector<std::string> ips = streamer_.clientIps();
    clientHeader_->setText(QStringLiteral("在线客户端 (%1)").arg(ips.size()));
    if (ips == lastClients_) return;
    lastClients_ = ips;

    // clear existing chips
    while (clientListLayout_->count() > 0) {
        QLayoutItem* it = clientListLayout_->takeAt(0);
        if (it->widget()) delete it->widget();
        delete it;
    }
    for (const auto& ip : ips)
        clientListLayout_->addWidget(makeClientCard(QString::fromStdString(ip)));
}

QWidget* TeacherWindow::makeClientCard(const QString& ip) {
    // a single client = one pill-shaped tag (person icon + IP), flowing in the layout
    auto* chip = new QLabel(QStringLiteral("👤 %1").arg(ip.toHtmlEscaped()));
    chip->setObjectName("clientChip");
    chip->setToolTip(ip);
    // inline stylesheet: consistent with the button / combo box approach (global QSS doesn't
    // always paint on Qt6/Fusion). A subtle pill with a clear border so it reads as a tag.
    chip->setStyleSheet(
        "QLabel{background-color:#1A2130;color:#C6CCE0;border:1px solid #2E3A55;"
        "border-radius:16px;padding:5px 14px;font-size:13px;}");
    return chip;
}

QString TeacherWindow::detectLocalIp() const {
    // Preferred: let the OS routing table pick the NIC. Connecting a UDP socket to a public
    // address performs route resolution WITHOUT sending any packet; the OS binds the socket to
    // the interface owning the default route (i.e. the one that actually reaches the internet),
    // and localAddress() reports that interface's IP. This correctly selects the real NIC even
    // when the machine has many adapters (VPN, VM, docker, etc.).
    {
        QUdpSocket probe;
        probe.connectToHost(QHostAddress(QStringLiteral("8.8.8.8")), 53);
        QHostAddress local = probe.localAddress();
        if (local.protocol() == QAbstractSocket::IPv4Protocol &&
            !local.isNull() && !local.isLoopback()) {
            QString ip = local.toString();
            if (!ip.isEmpty() && ip != QStringLiteral("0.0.0.0")) return ip;
        }
    }
    // Fallback: first up non-loopback IPv4 address (matches the old GetAdaptersInfo loop)
    for (const QNetworkInterface& iface : QNetworkInterface::allInterfaces()) {
        if (!(iface.flags() & QNetworkInterface::IsUp)) continue;
        if (iface.flags() & QNetworkInterface::IsLoopBack) continue;
        for (const QNetworkAddressEntry& entry : iface.addressEntries()) {
            QHostAddress a = entry.ip();
            if (a.protocol() == QAbstractSocket::IPv4Protocol && !a.isLoopback()) {
                QString ip = a.toString();
                if (ip != QStringLiteral("0.0.0.0")) return ip;
            }
        }
    }
    return QStringLiteral("127.0.0.1");
}