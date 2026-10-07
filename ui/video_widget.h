#pragma once
// ui/video_widget.h -- client video pane (Qt): wraps the D3D11Renderer inside a native QWidget.
// The renderer draws directly to the widget's HWND (WA_NativeWindow + WA_PaintOnScreen), so the
// D3D11 present() path is unchanged; Qt only owns the window lifetime and forwards input.
#include <QWidget>
#include <cstdint>
#include "render/d3d11_renderer.h"

class QMouseEvent;
class QWheelEvent;

class VideoWidget : public QWidget {
    Q_OBJECT
public:
    explicit VideoWidget(QWidget* parent = nullptr);
    ~VideoWidget() override;

    // (re)create the D3D11 device + swap chain against this widget's native HWND.
    // Called on first show and after the native window handle is recreated.
    bool initD3D();
    void shutdownD3D();

    D3D11Renderer* renderer() { return &renderer_; }

    // main-thread frame pump (called by the client window timer):
    // present a decoded BGRA frame, or repaint the cached frame (cursor keeps moving on
    // sparse/differential streams), or show the calm "waiting for teacher" placeholder.
    //
    // `buf` is taken by reference on purpose: the pixels are SWAPPED into this widget's one-frame
    // cache and `buf` comes back holding the previous cache, so the caller can hand that buffer back
    // to the frame pool. No full-frame copy happens anywhere in the display path.
    void presentFrame(std::vector<uint8_t>& buf, int w, int h);
    void presentCached();
    void presentNoSignal();
    // While paused (window minimized/hidden) frames are still cached but not uploaded to the GPU --
    // a 1080p texture map + memcpy 50x/s for a picture nobody can see is pure waste. Unpausing books
    // one repaint so the latest cached frame appears immediately.
    void setRenderPaused(bool p);

    // True exactly once after the widget was resized. The renderer only rebuilds its swap chain
    // inside present(), so a resize MUST be followed by one repaint -- otherwise the picture keeps
    // the old buffer size and gets stretched by the OS until the next frame happens to arrive
    // (which on a static desktop in differential mode can be up to 2s).
    bool consumeResizeRepaint() {
        const bool r = needsRepaint_;
        needsRepaint_ = false;
        return r;
    }
    // how many times the D3D device had to be torn down and rebuilt (driver reset / TDR / device
    // removal). Surfaced in the client title bar: a counter that keeps climbing means the graphics
    // stack is sick; a counter that stays put means the device is healthy. Cheap to expose, and it
    // settles "handles/threads jumped on that one machine" reports in one glance.
    int deviceRebuilds() const { return deviceRebuilds_; }

protected:
    void showEvent(QShowEvent* ev) override;
    void paintEvent(QPaintEvent* ev) override;   // no-op: D3D renders directly
    void resizeEvent(QResizeEvent* ev) override;
    void mousePressEvent(QMouseEvent* ev) override;
    void mouseReleaseEvent(QMouseEvent* ev) override;
    void mouseMoveEvent(QMouseEvent* ev) override;
    void wheelEvent(QWheelEvent* ev) override;
    void keyPressEvent(QKeyEvent* ev) override;
    void contextMenuEvent(QContextMenuEvent* ev) override;

private:
    void ensureNativeWindow();
    // (Re)create the D3D11 device if it isn't up -- or if the renderer reports the device was lost
    // (TDR / driver restart / sleep-resume). Rate-limited so a machine that simply cannot do D3D11
    // isn't hammered every frame. Returns true when a usable renderer exists.
    bool ensureRenderer();
    // A pan/zoom changes the picture with NO new frame involved, so it has to be redrawn right here or
    // it simply never appears. Measured before this existed: with the stream paused, a wheel zoom and a
    // 40px drag each changed 0 of 235848 sampled pixels -- the view moved internally and the screen
    // stayed put, which is exactly the reported "dragging does not follow the hand". Rate-limited to
    // ~the panel pace so a fast mouse cannot turn into a 500MB/s texture-upload loop; the frame pump's
    // idle tick picks up anything skipped.
    void requestViewRepaint();

    D3D11Renderer renderer_;
    bool d3dReady_ = false;
    bool needsRepaint_ = false;   // set by resizeEvent, consumed by consumeResizeRepaint()
    bool renderPaused_ = false;   // minimized/hidden: cache frames but don't upload them to the GPU
    int  deviceRebuilds_ = 0;     // device-loss recoveries (see deviceRebuilds())
    uint64_t lastInitAttemptMs_ = 0;   // rate limit for ensureRenderer()
    uint64_t lastViewPresentMs_ = 0;   // rate limit for requestViewRepaint()
    // cached last frame for sparse-stream repaint (owned here, moved from the frame pump)
    std::vector<uint8_t> lastBgra_;
    int lastFW_ = 0, lastFH_ = 0;
};