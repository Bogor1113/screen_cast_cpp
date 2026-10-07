#pragma once
// capture/dxgi_capture.h -- DXGI Desktop Duplication screen capture
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <dxgi1_2.h>
#include <atomic>
#include <cstdint>
#include <functional>
#include <mutex>
#include <vector>

// one raw frame (system memory, BGRA8888) + changed-region info (for differential screen cast)
struct CapturedFrame {
    const uint8_t* data = nullptr;      // internal buffer, valid only during the callback
    int width = 0, height = 0;
    int stride = 0;                     // bytes per row (may exceed width*4)
    uint64_t ptsMs = 0;                 // capture time (ms)
    std::vector<RECT> dirtyRects;       // changed regions (screen coords), for differential judgment/visualization
    uint64_t dirtyArea = 0;             // total changed-region pixels (first frame = full screen)
};

// DXGI desktop duplication: only produces a new frame when the picture changes (follows the monitor refresh rate)
class DXGICapture {
public:
    using FrameCallback = std::function<void(const CapturedFrame&)>;

    DXGICapture() = default;
    ~DXGICapture();

    bool start(int outputIndex, FrameCallback cb);   // 0 = primary monitor
    void stop();
    bool pumpFrame();    // capture thread calls in a loop; callback true on a new frame, false on no change
    // exclude the own-window region (screen coords): that region is filled with a fixed color and deducted from the dirty
    // rects, eliminating "picture-in-picture" feedback flicker, and letting a static desktop trigger differential skip
    void setExcludeScreenRect(const RECT& rc, bool enabled);
    bool isRunning() const { return running_; }
    // GDI fallback: under remote desktop/virtual display DXGI may capture black, auto-switch to GDI screen grab
    bool gdiMode() const { return gdiMode_.load(); }
    // a client just asked for a keyframe (control channel): the capture thread's next pumpFrame()
    // (<=2ms away) must produce a frame immediately instead of waiting out the 2s keep-alive -- and,
    // on a path that has never delivered anything (dead DXGI), the GDI fallback probe is brought
    // forward from 3s to ~0.8s so a joining client does not stare at "waiting for teacher".
    void requestFrameSoon() { forceEmit_.store(true); }

private:
    bool createDeviceAndDuplication(int outputIndex);
    void releaseResources();
    void applyExclude(uint8_t* data, CapturedFrame& f, int w, int h, int stride);
    bool createGdiResources();
    void destroyGdiResources();
    int  gdiProbeBrightness();
    int  sampleBrightness(const uint8_t* data, int stride, int w, int h);
    bool detectBlackSwitch();          // DXGI black-screen probe: on anomaly auto-switch to GDI, returns true = switched
    bool pumpFrameGdiLocked();
    // GDI has no dirty-rect metadata, so a sparse sample-grid hash stands in for it: an unchanged
    // grid means "nothing moved" and lets the streamer skip encoding a static desktop.
    static uint64_t gdiFrameHash(const uint8_t* data, int w, int h, int stride);

    ID3D11Device*           device_ = nullptr;
    ID3D11DeviceContext*    context_ = nullptr;
    IDXGIOutputDuplication* duplication_ = nullptr;
    ID3D11Texture2D*        stagingTex_[2] = {};   // double buffer, avoid CPU waiting on GPU copy
    int                     stagingIdx_ = 0;
    std::vector<uint8_t>    readbackBuf_;
    // set by Streamer when a client asks for a keyframe: emit a frame on the next pump instead of
    // waiting out the 2s keep-alive interval, so a joining client gets a picture immediately rather
    // than "as soon as the teacher happens to move the mouse".
    std::atomic<bool>       forceEmit_{ false };
    int64_t                 firstPumpMs_ = 0;    // first pump time (lazy): "DXGI never delivered anything" timer
    std::vector<uint8_t>    moveBuf_;    // move rects metadata
    std::vector<uint8_t>    dirtyBuf_;   // dirty rects metadata
    int                     outW_ = 0, outH_ = 0, outStride_ = 0;
    int                     outX_ = 0, outY_ = 0;  // output origin in the virtual desktop (DesktopCoordinates.left/top)
    RECT                    excludeRect_{ 0, 0, 0, 0 };  // excluded region (screen coords) — separate lock, no competition with capture
    std::atomic<bool>       excludeEnabled_{ false };      // cross-thread atomic read/write, lock-free
    std::mutex              excludeMtx_;                    // only protects read/write of excludeRect_
    int64_t                 lastEmitMs_ = 0;      // time of the last emitted frame (for static keep-alive)
    bool                    haveFrame_ = false;   // whether there's a reusable read-back buffer
    int                     outputIndex_ = 0;      // remember the monitor, reused when rebuilding on ACCESS_LOST
    FrameCallback           cb_;
    std::mutex              mtx_;
    std::atomic<bool>       running_{ false };

    // GDI fallback resources (remote/virtual display env)
    HDC                     gdiScreenDC_ = nullptr, gdiMemDC_ = nullptr;
    HBITMAP                 gdiDib_ = nullptr;
    HGDIOBJ                 gdiOld_ = nullptr;
    uint8_t*                gdiBits_ = nullptr;
    BITMAPINFO              gdiBmi_{};
    std::atomic<bool>       gdiMode_{ false };    // whether currently in GDI mode (cross-thread snapshot read)
    int64_t                 lastBlackCheckMs_ = 0;  // last black-screen probe time (timer-triggered, not frame-rate dependent)
    bool                    firstFrameChecked_ = false;  // first frame already black-probed (black-on-open judged instantly)
    int64_t                 lastRecreateMs_ = 0;   // ACCESS_LOST rebuild throttle (retry every 500ms on failure)
    int64_t                 lastGdiEmitMs_ = 0;    // GDI mode frame-rate limit
    uint64_t                lastGdiHash_ = 0;      // sample-grid hash of the previous GDI grab (change detection)
};
