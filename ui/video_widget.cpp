// ui/video_widget.cpp -- client video pane: D3D11Renderer embedded in a native QWidget
#include "ui/video_widget.h"
#include "ui/qss_dark.h"
#include <QShowEvent>
#include <QPaintEvent>
#include <QResizeEvent>
#include <QMouseEvent>
#include <QWheelEvent>
#include <QKeyEvent>
#include <QContextMenuEvent>
#include <QMenu>
#include <QCursor>
#include <algorithm>   // std::max in initD3D
#include <chrono>

static uint64_t steadyMsLocal() {
    return (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
        std::chrono::steady_clock::now().time_since_epoch()).count();
}

VideoWidget::VideoWidget(QWidget* parent) : QWidget(parent) {
    // D3D renders straight to the widget's native HWND; Qt must not composite the widget itself
    setAttribute(Qt::WA_NativeWindow, true);
    setAttribute(Qt::WA_PaintOnScreen, true);
    setAttribute(Qt::WA_OpaquePaintEvent, true);
    setAttribute(Qt::WA_NoSystemBackground, true);
    setFocusPolicy(Qt::StrongFocus);
    setMouseTracking(true);
    // "There is a picture here you can grab and move": open hand when idle over the video, closed hand
    // while dragging. The renderer's own WM_SETCURSOR handler never applies to the Qt client (Qt owns
    // the window, so the renderer never installs a window procedure), which is why the cursor used to
    // stay an arrow through the whole drag.
    setCursor(Qt::OpenHandCursor);
}

VideoWidget::~VideoWidget() { shutdownD3D(); }

void VideoWidget::ensureNativeWindow() {
    (void)winId();   // forces the native HWND to exist
}

bool VideoWidget::initD3D() {
    ensureNativeWindow();
    if (d3dReady_) return true;
    int w = std::max(width(), 1), h = std::max(height(), 1);
    // createForHwnd: device + swap chain against our HWND; the window stays Qt-owned
    if (!renderer_.createForHwnd((HWND)winId(), w, h)) return false;
    d3dReady_ = true;
    return true;
}

void VideoWidget::shutdownD3D() {
    if (d3dReady_) {
        renderer_.destroy();
        d3dReady_ = false;
    }
}

bool VideoWidget::ensureRenderer() {
    // Rebuild the device when it went away (driver reset / TDR / sleep-resume / GPU switch). Without
    // this the renderer simply kept failing on a dead device and the student watched a frozen or black
    // window for the rest of the lesson with no way to recover but restarting the client.
    if (renderer_.takeDeviceLost()) {
        renderer_.destroy();
        d3dReady_ = false;
        ++deviceRebuilds_;
    }
    if (d3dReady_ && renderer_.valid()) return true;
    const uint64_t now = steadyMsLocal();
    if (now - lastInitAttemptMs_ < 2000) return false;   // don't hammer a machine that can't do D3D11
    lastInitAttemptMs_ = now;
    return initD3D();
}

void VideoWidget::setRenderPaused(bool p) {
    if (p == renderPaused_) return;
    renderPaused_ = p;
    if (!p) needsRepaint_ = true;   // unpausing: repaint the latest cached frame at once
}

void VideoWidget::requestViewRepaint() {
    if (!renderer_.takeViewDirty()) return;   // nothing about the view changed
    needsRepaint_ = true;                     // the idle tick finishes the job if we skip now
    if (renderPaused_) return;
    const uint64_t now = steadyMsLocal();
    if (now - lastViewPresentMs_ < 15) return;   // cap at ~the panel pace
    lastViewPresentMs_ = now;
    presentCached();                          // clears needsRepaint_
}

void VideoWidget::showEvent(QShowEvent* ev) {
    QWidget::showEvent(ev);
    ensureRenderer();
}

void VideoWidget::paintEvent(QPaintEvent*) {
    // A WA_PaintOnScreen widget is never composed from Qt's backing store, so an OS-requested repaint
    // MUST be answered here by drawing again -- Qt has nothing cached to blit. The old no-op left the
    // invalidated region showing whatever the swap chain happened to hold: with the bitblt-model
    // chain the front buffer is undefined after Present, so the region went BLACK, and in differential
    // mode no new frame may arrive for up to 2s (static desktop) -> the student stared at a black patch
    // until the next frame, then it came back, then the next menu event blacked it again ("flickers and
    // goes black when picking a right-click menu item"). Re-presenting the cached frame costs one draw
    // (no decode, no upload) and makes every such event self-healing.
    if (renderPaused_) return;
    if (lastFW_ > 0 && !lastBgra_.empty()) presentCached();
    else presentNoSignal();
}

void VideoWidget::resizeEvent(QResizeEvent* ev) {
    QWidget::resizeEvent(ev);
    // the renderer picks up the new client size in its next present() (GetClientRect + ResizeBuffers),
    // so book a repaint to guarantee that present() actually happens
    needsRepaint_ = true;
}

void VideoWidget::mousePressEvent(QMouseEvent* ev) {
    if (ev->button() == Qt::LeftButton) {
        // DPI: Qt reports LOGICAL coordinates while the renderer lives in PHYSICAL pixels
        // (GetClientRect + swap chain). On a 150% display the raw logical position made every drag
        // move the picture at only 1/dpr of the hand speed -- students described it as "the picture
        // scrolls instead of following the hand and never stops where I drop it". Scale once here so
        // press/move/release/wheel all speak physical pixels to the renderer.
        const QPointF p = ev->position() * devicePixelRatio();
        renderer_.onLButtonDown(p.x(), p.y());
        setCursor(Qt::ClosedHandCursor);   // grabbing
        grabMouse();   // Qt-side capture (the Win32 SetCapture was removed from the renderer)
    }
    QWidget::mousePressEvent(ev);
}

void VideoWidget::mouseReleaseEvent(QMouseEvent* ev) {
    if (ev->button() == Qt::LeftButton) {
        renderer_.onLButtonUp();
        setCursor(Qt::OpenHandCursor);     // back to "you can grab it"
        releaseMouse();
    }
    QWidget::mouseReleaseEvent(ev);
}

void VideoWidget::mouseMoveEvent(QMouseEvent* ev) {
    const QPointF p = ev->position() * devicePixelRatio();   // logical -> physical, see mousePressEvent
    renderer_.onMouseMove(p.x(), p.y(),
                          (ev->buttons() & Qt::LeftButton) ? MK_LBUTTON : 0);
    requestViewRepaint();   // the drag moved the picture: show it now, not on the next frame
    QWidget::mouseMoveEvent(ev);
}

void VideoWidget::wheelEvent(QWheelEvent* ev) {
    // zoom at the mouse position; delta in eighths of a degree, 120 per notch
    int delta = ev->angleDelta().y();
    if (delta != 0) {
        const QPointF p = ev->position() * devicePixelRatio();   // logical -> physical
        renderer_.onMouseWheel(delta, p.x(), p.y());
        requestViewRepaint();
    }
    ev->accept();
}

void VideoWidget::keyPressEvent(QKeyEvent* ev) {
    // keep in sync with D3D11Renderer::onKeyDown and with the context-menu labels
    switch (ev->key()) {
    case Qt::Key_1: renderer_.nativeView(); requestViewRepaint(); ev->accept(); return;   // 1:1 原始大小
    case Qt::Key_0:
    case Qt::Key_2:
    case Qt::Key_Home: renderer_.resetView(); requestViewRepaint(); ev->accept(); return; // 适应窗口
    case Qt::Key_Plus:
    case Qt::Key_Equal: renderer_.zoomIn(); requestViewRepaint(); ev->accept(); return;
    case Qt::Key_Minus: renderer_.zoomOut(); requestViewRepaint(); ev->accept(); return;
    default: break;
    }
    QWidget::keyPressEvent(ev);
}

void VideoWidget::contextMenuEvent(QContextMenuEvent* ev) {
    QMenu menu(this);
    // widget-level stylesheet: the QMenu rule in the app-wide stylesheet is ignored under
    // Fusion (the menu takes its background from the system palette), which produced a white
    // menu with near-white text on machines running Windows in light mode.
    menu.setStyleSheet(kPopupMenuQss);
    QAction* a100 = menu.addAction(QStringLiteral("1:1 原始大小\t1"));
    QAction* aFit = menu.addAction(QStringLiteral("适应窗口\t0"));
    menu.addSeparator();
    QAction* aIn = menu.addAction(QStringLiteral("放大\t+"));
    QAction* aOut = menu.addAction(QStringLiteral("缩小\t-"));
    QAction* picked = menu.exec(ev->globalPos());
    // The popup overlapped the D3D surface; its dismissal invalidates that region. Book a repaint even
    // when the chosen action changed nothing (picking "适应窗口" while already fitted), otherwise the
    // region stays stale until an unrelated frame happens to arrive.
    needsRepaint_ = true;
    if (!picked) return;
    if (picked == a100) renderer_.nativeView();
    else if (picked == aFit) renderer_.resetView();
    else if (picked == aIn) renderer_.zoomIn();
    else if (picked == aOut) renderer_.zoomOut();
    requestViewRepaint();   // same reason as the wheel/drag paths: nothing else would redraw it
}

// ---------------- main-thread frame pump (called by the client window timer) ----------------

void VideoWidget::presentFrame(std::vector<uint8_t>& buf, int w, int h) {
    if (buf.empty() || w <= 0 || h <= 0) return;
    const size_t need = (size_t)w * h * 4;
    if (buf.size() < need) return;   // short buffer would be read out of bounds by the renderer
    if (ensureRenderer()) {
        renderer_.setNoSignal(false);   // real decoded picture
        if (!renderPaused_) renderer_.present(buf.data(), w, h);
        needsRepaint_ = renderPaused_;  // paused frames weren't drawn: keep the repaint booked
    }
    // Swap (not copy) the pixels into the cache: `buf` leaves here holding the previous cache, which
    // the caller recycles into the frame pool. This is what makes the steady state allocation-free.
    lastBgra_.swap(buf);
    lastFW_ = w; lastFH_ = h;
}

void VideoWidget::presentCached() {
    if (lastFW_ <= 0 || lastFH_ <= 0 || lastBgra_.empty()) return;
    if (!ensureRenderer()) return;
    if (renderPaused_) return;
    renderer_.setNoSignal(false);   // real cached picture, not the waiting placeholder
    renderer_.present(lastBgra_.data(), lastFW_, lastFH_);
    needsRepaint_ = false;
}

void VideoWidget::presentNoSignal() {
    if (!ensureRenderer()) return;
    if (renderPaused_) return;
    // calm static dark-slate placeholder (no harsh blinking), refreshed at ~2.5Hz so
    // window resizes get a fresh buffer; the title bar carries the real status text
    const int nw = 1280, nh = 720;
    static std::vector<uint8_t> nosig;
    if (nosig.size() != (size_t)nw * nh * 4) {
        nosig.assign((size_t)nw * nh * 4, 0);
        for (size_t i = 0; i < nosig.size(); i += 4) {
            nosig[i + 0] = 76; nosig[i + 1] = 34; nosig[i + 2] = 26; nosig[i + 3] = 255;
        }
    }
    renderer_.setNoSignal(true);   // not a real picture: no drag-hand cursor over it
    renderer_.present(nosig.data(), nw, nh);
}