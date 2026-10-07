#pragma once
// render/d3d11_renderer.h -- client display window (pure Win32 + D3D11, no external deps)
#ifndef WIN32_LEAN_AND_MEAN
#define WIN32_LEAN_AND_MEAN
#endif
#ifndef NOMINMAX
#define NOMINMAX
#endif
#include <windows.h>
#include <d3d11.h>
#include <cstdint>

// window class: creates the window + swap chain + texture rendering
// uses wide-character APIs throughout, title supports Chinese
class D3D11Renderer {
public:
    D3D11Renderer() = default;
    ~D3D11Renderer();

    // title: window title (wide char, supports Chinese); w/h: initial size; returns false on init failure
    bool create(const wchar_t* title, int w, int h);
    // Qt integration: create the D3D11 device + swap chain against an EXISTING window (a Qt
    // widget's native HWND). The window is owned by the caller (Qt); the renderer only draws.
    bool createForHwnd(HWND hwnd, int w, int h);
    void destroy();
    void onClose();              // WM_CLOSE handling
    void onLButtonDown(int x, int y);
    void onLButtonUp();
    void onMouseMove(int x, int y, WPARAM keys);
    void onMouseWheel(int delta, int x, int y);
    void onKeyDown(WPARAM key);
    void onContextMenu(int x, int y);     // right-click menu

    // zoom/pan API (right-click menu / shortcut keys)
    void resetView();        // fit window: zoom=1, pan=0
    void nativeView();       // 1:1: source pixel = screen pixel
    void zoomIn();           // zoom in 25%
    void zoomOut();          // zoom out 25%
    float currentZoom() const { return zoom_; }
    float effectiveZoom() const;   // currently effective zoom factor (zoom_ x base fit ratio, shown in title bar)
    // current pan offset in window pixels (title-bar diagnostic: a student reporting "my picture
    // wandered off" can read the offset directly; also lets automated tests assert drag persistence)
    float panX() const { return panX_; }
    float panY() const { return panY_; }

    // teacher's captured source resolution (for a true 1:1 even when the stream is downscaled); 0 = unknown
    void setSourceSize(int w, int h) { srcW_ = w; srcH_ = h; }

    // ---- view-change notification ----------------------------------------------------------------
    // Pan/zoom change the picture without any new frame being involved. The owner polls this and
    // repaints the cached frame; without it a drag only became visible when something unrelated
    // triggered a repaint (a newly arrived frame, or the teacher's pointer moving) -- so dragging
    // looked dead precisely when the teacher's hand was off the mouse.
    bool takeViewDirty() { const bool d = viewDirty_; viewDirty_ = false; return d; }

    // teacher pointer (normalized 0..1, relative to captured picture): called from main thread, drawn as overlay during render
    void setCursor(float nx, float ny, bool visible);

    // dynamically update the title bar (e.g. status info), wide char
    void setTitle(const wchar_t* title);

    // draw one BGRA frame to the window (keep aspect ratio, letterbox with black)
    void present(const uint8_t* bgra, int imgW, int imgH);

    // True exactly once after the D3D device was lost (driver reset / TDR / sleep-resume / GPU switch),
    // or after the swap chain failed to resize repeatedly. The owner polls this and rebuilds the device:
    // without it the renderer just keeps presenting into a dead swap chain and the student stares at a
    // frozen/black window until the client is restarted.
    bool takeDeviceLost() { const bool l = deviceLost_; deviceLost_ = false; return l; }
    // true when the device fell back to WARP (the D3D11 software rasterizer). Classroom PCs / VMs / RDP
    // sessions frequently have no usable 3D driver; WARP keeps the student seeing a picture instead of
    // a blank window, at the cost of CPU. Surfaced in the client title bar for diagnosis.
    bool usingWarp() const { return warp_; }

    // message pump: returns false when the window has been closed
    bool pumpMessages();
    bool valid() const { return hwnd_ != nullptr && dev_ != nullptr && swap_ != nullptr; }
    // whether a real video frame is currently displayed (vs. the "waiting for teacher" placeholder)
    bool hasVideo() const { return vidW_ > 0 && vidH_ > 0 && !noSignal_; }
    // size of the decoded frame currently on screen, and the swap-chain size it is being drawn into.
    // Surfaced in the client title bar: it is the fastest way to tell whether the picture looks soft
    // because the teacher downscaled, or because the render surface is not the size we think it is.
    int  videoW()  const { return vidW_; }
    int  videoH()  const { return vidH_; }
    int  surfaceW() const { return clientW_; }
    int  surfaceH() const { return clientH_; }
    // mark the currently displayed frame as the "waiting for teacher" placeholder (no real picture yet)
    void setNoSignal(bool ns) { noSignal_ = ns; }
    // whether the teacher's remote pointer overlay was drawn inside the window by the last present()
    // (informational only; the student's own local cursor is NEVER hidden based on this -- hiding it made
    // the student lose sight of their own pointer, reported as "the picture swallowed my mouse")
    bool remoteOverlayInView() const { return cursorWinVisible_; }
    // whether the user is currently dragging the picture (WM_SETCURSOR shows the drag-hand only then,
    // so the student always sees their own normal arrow otherwise)
    bool isDragging() const { return dragging_; }

private:
    bool createDeviceAndSwapChain(int w, int h);
    void initShaders();            // one-time shader/sampler init
    void releaseD3D();             // release every D3D object (keeps the window); used by destroy + WARP retry

    HWND   hwnd_ = nullptr;
    bool   ownsWindow_ = false;   // true: we created the HWND (create); false: Qt owns it (createForHwnd)
    bool   warp_ = false;         // device created via D3D_DRIVER_TYPE_WARP (software rasterizer)
    // swap chain uses the FLIP model (DXGI_SWAP_EFFECT_FLIP_DISCARD): buffers rotate on every Present,
    // so the render target view must be re-acquired after each Present (see present())
    bool   flip_ = false;
    bool   deviceLost_ = false;   // set on DXGI_ERROR_DEVICE_REMOVED; consumed by takeDeviceLost()
    int    resizeFailStreak_ = 0; // consecutive ResizeBuffers failures (10 in a row => dead device)
    int    cursorTexTries_ = 0;   // capped retries for the (rare) cursor-texture build failure
    int    shaderTries_ = 0;      // capped retries for the D3DCompile step
    ID3D11Device*        dev_ = nullptr;
    ID3D11DeviceContext* ctx_ = nullptr;
    IDXGISwapChain*      swap_ = nullptr;
    ID3D11RenderTargetView* rtv_ = nullptr;
    ID3D11Texture2D*     backBuffer_ = nullptr;
    ID3D11Texture2D*     videoTex_ = nullptr;   // video frame texture (BGRA8)
    ID3D11ShaderResourceView* srv_ = nullptr;
    ID3D11VertexShader*  vs_ = nullptr;
    ID3D11PixelShader*   ps_ = nullptr;
    ID3D11InputLayout*   layout_ = nullptr;
    ID3D11Buffer*        vb_ = nullptr;
    ID3D11SamplerState*  sampler_ = nullptr;
    // cursor overlay: separate small texture + vertex buffer (drawn on top after the main draw)
    ID3D11Texture2D*     cursorTex_ = nullptr;
    ID3D11ShaderResourceView* cursorSrv_ = nullptr;
    ID3D11Buffer*        cursorVB_ = nullptr;
    ID3D11BlendState*    blendAlpha_ = nullptr;   // alpha blend for the cursor overlay (transparent pixels don't cover with black blocks)
    int  cursorTexW_ = 0, cursorTexH_ = 0;
    int  cursorHotX_ = 0, cursorHotY_ = 0;        // the real cursor's hotspot pixel offset inside the texture (hotspot=pointer tip)
    bool haveCursor_ = false;       // a teacher cursor packet has been received (position valid)
    float cursorNX_ = 0.0f, cursorNY_ = 0.0f;   // normalized coords
    bool  cursorVisible_ = false;   // teacher pointer visible
    int  clientW_ = 0, clientH_ = 0;
    int  vidW_ = 0, vidH_ = 0;
    int  srcW_ = 0, srcH_ = 0;   // teacher captured source resolution (0 = unknown, fall back to decoded size)
    bool closed_ = false;
    // zoom/pan
    float zoom_ = 1.0f;         // 1.0=fit window, >1 zoom in
    float panX_ = 0.0f, panY_ = 0.0f;  // pan offsets (pixels)
    bool  dragging_ = false;
    bool  viewDirty_ = false;   // set by pan/zoom, consumed by takeViewDirty()
    int   dragStartMX_ = 0, dragStartMY_ = 0;
    float dragStartPX_ = 0.0f, dragStartPY_ = 0.0f;
    // cursor-overlay drawing helpers
    bool  ensureCursorTexture();
    bool  ensureBlendState();
    void  drawCursorOverlay(float es, float scx, float scy, float halfVW, float halfVH);

    // updated by drawCursorOverlay each present: whether the remote pointer was actually drawn in view
    bool  cursorWinVisible_ = false;
    bool  noSignal_ = false;   // true while showing the "waiting for teacher" placeholder (not a real picture)
};
