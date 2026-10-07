// render/d3d11_renderer.cpp -- D3D11 renderer implementation (fullscreen quad + sampled texture, zoom/pan)
#include "render/d3d11_renderer.h"
#include <dxgi1_2.h>   // IDXGIFactory2 + DXGI_SWAP_CHAIN_DESC1 (flip-model swap chain creation)
#include <d3dcompiler.h>
#include <cstdio>
#include <cstring>
#include <vector>
#include <algorithm>

struct Vertex { float x, y, u, v; };

D3D11Renderer::~D3D11Renderer() { destroy(); }

static LRESULT CALLBACK rendererWndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    D3D11Renderer* self = (D3D11Renderer*)GetWindowLongPtrW(hwnd, GWLP_USERDATA);
    if (msg == WM_NCCREATE) {
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
        SetWindowLongPtrW(hwnd, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return TRUE;
    }
    if (self) {
        switch (msg) {
        case WM_CLOSE:    self->onClose(); return 0;
        case WM_LBUTTONDOWN: self->onLButtonDown(LOWORD(lp), HIWORD(lp)); return 0;
        case WM_LBUTTONUP:   self->onLButtonUp(); return 0;
        case WM_MOUSEMOVE:   self->onMouseMove(LOWORD(lp), HIWORD(lp), wp); return 0;
        case WM_MOUSEWHEEL: {
            // the wheel message's coords are screen coords; must convert to client coords before giving
            // them to the zoom logic (otherwise when the window isn't at the screen top-left, the zoom
            // center shifts -- seen as "wheel zoom isn't at the mouse position")
            POINT pt{ (short)LOWORD(lp), (short)HIWORD(lp) };
            ScreenToClient(hwnd, &pt);
            self->onMouseWheel(GET_WHEEL_DELTA_WPARAM(wp), pt.x, pt.y);
            return 0;
        }
        case WM_SETCURSOR: {
            // The student must ALWAYS see their own local hardware cursor. The old code hid it wherever
            // the teacher's remote pointer overlay was drawn (SetCursor(nullptr)) and forced a drag-hand
            // over the picture -- on the student machine that read as "the picture swallowed my mouse / I
            // can't tell where I'm pointing". The teacher's remote pointer is just a picture overlay now;
            // the student's own cursor is never hidden, and the drag-hand only shows while actually dragging.
            if (LOWORD(lp) == HTCLIENT && self->isDragging()) {
                SetCursor(LoadCursor(nullptr, IDC_HAND));
                return TRUE;
            }
            break;   // everything else falls through to DefWindowProc -> normal arrow
        }
        case WM_KEYDOWN:     self->onKeyDown(wp); return 0;
        case WM_CONTEXTMENU: self->onContextMenu(LOWORD(lp), HIWORD(lp)); return 0;
        }
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

void D3D11Renderer::onClose() { closed_ = true; DestroyWindow(hwnd_); hwnd_ = nullptr; PostQuitMessage(0); }
bool D3D11Renderer::pumpMessages() {
    MSG msg;
    while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
        if (msg.message == WM_QUIT) { closed_ = true; return false; }
        TranslateMessage(&msg); DispatchMessageW(&msg);
    }
    return !closed_;
}

// mouse/zoom handlers --------------------------------------------------------
void D3D11Renderer::onLButtonDown(int x, int y) {
    // free drag at ANY zoom level: grab the picture and move it wherever the mouse goes
    // ("随意拖拽"). At fit view the picture slides within the window (its center stays inside,
    // clamped in present()); zoomed in it pans the visible region. If there's no picture yet
    // (no video frame), there's nothing to drag, ignore.
    if (!hasVideo()) return;
    dragging_ = true; dragStartMX_ = x; dragStartMY_ = y;
    dragStartPX_ = panX_; dragStartPY_ = panY_;
}
void D3D11Renderer::onLButtonUp() { dragging_ = false; }
void D3D11Renderer::onMouseMove(int x, int y, WPARAM keys) {
    if (!dragging_) return;
    if (!(keys & MK_LBUTTON)) { dragging_ = false; return; }
    // GRAB semantics (the universal image-pan convention -- Qt QGraphicsView ScrollHandDrag does
    // `scrollbar->setValue(value() - dx)`, Windows Photos / IrfanView / map apps are the same):
    // the pixel under the cursor must stay under the cursor, so the visible-region offset moves
    // OPPOSITE to the cursor. The old `+ delta` produced scrollbar semantics -- drag right and the
    // picture ran left -- which reads as "scrolling", not "carrying the picture".
    float nx = dragStartPX_ - (float)(x - dragStartMX_);
    float ny = dragStartPY_ - (float)(y - dragStartMY_);
    if (nx == panX_ && ny == panY_) return;   // no actual movement: don't ask for a pointless repaint
    panX_ = nx; panY_ = ny;
    viewDirty_ = true;                        // the picture moved: the owner must redraw the cached frame
}
void D3D11Renderer::onMouseWheel(int delta, int x, int y) {
    float factor = delta > 0 ? 1.12f : (1.0f / 1.12f);
    float newZoom = zoom_ * factor;
    if (newZoom < 0.25f) newZoom = 0.25f; if (newZoom > 20.0f) newZoom = 20.0f;
    float bs = (clientW_ > 0 && vidW_ > 0) ? fmin((float)clientW_ / vidW_, (float)clientH_ / vidH_) : 1.0f;
    float oldEff = zoom_ * bs, newEff = newZoom * bs;
    if (oldEff > 0 && newEff > 0) {
        float cx = x - clientW_ * 0.5f, cy = y - clientH_ * 0.5f;
        panX_ = cx + (panX_ - cx) * newEff / oldEff;
        panY_ = cy + (panY_ - cy) * newEff / oldEff;
    }
    if (newZoom == zoom_) return;
    zoom_ = newZoom;
    viewDirty_ = true;
}
void D3D11Renderer::onKeyDown(WPARAM key) {
    // Semantics must match VideoWidget::keyPressEvent and the right-click menu labels. The two used
    // to disagree ('1' meant "fit window" here but "1:1" on the Qt client), so the same key did
    // opposite things depending on which layer handled it.
    switch (key) {
    case '1': nativeView(); break;                          // 1   = 1:1 原始大小
    case '0': case '2': resetView(); break;                 // 0/2 = 适应窗口
    case VK_OEM_PLUS: case VK_ADD: case '=': zoomIn(); break;
    case VK_OEM_MINUS: case VK_SUBTRACT: case '-': zoomOut(); break;
    }
}

void D3D11Renderer::setCursor(float nx, float ny, bool visible) {
    if (nx < 0.0f || nx > 1.0f || ny < 0.0f || ny > 1.0f) { haveCursor_ = false; return; }
    cursorNX_ = nx; cursorNY_ = ny; cursorVisible_ = visible; haveCursor_ = true;
}

float D3D11Renderer::effectiveZoom() const {
    float bs = (clientW_ > 0 && vidW_ > 0) ? fmin((float)clientW_ / vidW_, (float)clientH_ / vidH_) : 1.0f;
    return zoom_ * bs;
}

void D3D11Renderer::resetView() {
    if (zoom_ == 1.0f && panX_ == 0.0f && panY_ == 0.0f) return;
    zoom_ = 1.0f; panX_ = 0.0f; panY_ = 0.0f;
    viewDirty_ = true;
}
void D3D11Renderer::nativeView() {
    // 1:1 original resolution = source pixel = screen pixel (zoom = 1/baseScale).
    // Use the teacher's captured SOURCE size (srcW_/srcH_), not the decoded stream size: when the
    // teacher downscales (HD1080P/SD720P/FLUENT) the streamed frame is smaller than the source, so a
    // 1:1 computed from the decoded size shows "too small / inaccurate". Fall back to the decoded size
    // while the source size hasn't arrived yet.
    int w = srcW_ > 0 ? srcW_ : vidW_;
    int h = srcH_ > 0 ? srcH_ : vidH_;
    float bs = (clientW_ > 0 && w > 0) ? fmin((float)clientW_ / w, (float)clientH_ / h) : 1.0f;
    float nz = bs > 0 ? 1.0f / bs : 1.0f;
    if (nz == zoom_ && panX_ == 0.0f && panY_ == 0.0f) return;
    zoom_ = nz;
    panX_ = 0.0f; panY_ = 0.0f;
    viewDirty_ = true;
}
void D3D11Renderer::zoomIn() {
    float newZoom = zoom_ * 1.25f;
    if (newZoom > 20.0f) newZoom = 20.0f;
    if (newZoom == zoom_) return;
    zoom_ = newZoom; panX_ *= 0.8f; panY_ *= 0.8f;  // move slightly toward center when zooming in
    viewDirty_ = true;
}
void D3D11Renderer::zoomOut() {
    float newZoom = zoom_ * 0.8f;
    if (newZoom < 0.25f) newZoom = 0.25f;   // minimum zoom-out to 25%, can see the full picture letterboxed
    if (newZoom == zoom_) return;
    zoom_ = newZoom; panX_ *= 1.25f; panY_ *= 1.25f;
    viewDirty_ = true;
}

void D3D11Renderer::onContextMenu(int x, int y) {
    HMENU hMenu = CreatePopupMenu();
    AppendMenuW(hMenu, MF_STRING, 1001, L"1:1 原始分辨率(&2)");    // nativeView
    AppendMenuW(hMenu, MF_STRING, 1002, L"适应窗口(&1)");         // resetView
    AppendMenuW(hMenu, MF_SEPARATOR, 0, nullptr);
    AppendMenuW(hMenu, MF_STRING, 1003, L"放大 +(=)");             // zoomIn
    AppendMenuW(hMenu, MF_STRING, 1004, L"缩小 -(-)");             // zoomOut
    POINT pt{ x, y };
    ClientToScreen(hwnd_, &pt);
    int id = TrackPopupMenu(hMenu, TPM_RIGHTBUTTON | TPM_RETURNCMD, pt.x, pt.y, 0, hwnd_, nullptr);
    DestroyMenu(hMenu);
    switch (id) {
    case 1001: nativeView(); break;
    case 1002: resetView(); break;
    case 1003: zoomIn(); break;
    case 1004: zoomOut(); break;
    }
}

// create / destroy -----------------------------------------------------------
bool D3D11Renderer::create(const wchar_t* title, int w, int h) {
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    WNDCLASSW wc = {};
    wc.lpfnWndProc = rendererWndProc; wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(hInst, L"APPICON");                    // custom embedded icon (msi/student.ico)
    wc.hbrBackground = (HBRUSH)GetStockObject(BLACK_BRUSH);
    wc.lpszClassName = L"ScreenCastClient"; RegisterClassW(&wc);
    hwnd_ = CreateWindowW(L"ScreenCastClient", title, WS_OVERLAPPEDWINDOW,
                          CW_USEDEFAULT, CW_USEDEFAULT, w, h, nullptr, nullptr, hInst, this);
    if (!hwnd_) return false;
    ownsWindow_ = true;
    ShowWindow(hwnd_, SW_SHOW);
    if (!createDeviceAndSwapChain(w, h)) { destroy(); return false; }
    return true;
}
bool D3D11Renderer::createForHwnd(HWND hwnd, int w, int h) {
    if (!hwnd) return false;
    hwnd_ = hwnd;
    ownsWindow_ = false;   // the window is owned by Qt; destroy() must not DestroyWindow it
    if (!createDeviceAndSwapChain(w, h)) { destroy(); return false; }
    return true;
}
void D3D11Renderer::setTitle(const wchar_t* title) {
    if (hwnd_ && title) SetWindowTextW(hwnd_, title);
}

bool D3D11Renderer::createDeviceAndSwapChain(int w, int h) {
    if (w <= 0 || h <= 0) return false;

    // Field override for machines whose GPU driver is broken in a way that creates a device but
    // renders garbage (the automatic fallback below only catches a FAILED creation). Setting
    // SCREENCAST_FORCE_WARP=1 on the student machine switches rendering to the CPU rasterizer with no
    // rebuild and no shortcut edit -- the same switch is used to exercise that path in testing.
    bool forceWarp = false;
    {
        char buf[8] = { 0 };
        DWORD n = GetEnvironmentVariableA("SCREENCAST_FORCE_WARP", buf, sizeof(buf));
        forceWarp = (n > 0 && buf[0] == '1');
    }

    UINT flags = D3D11_CREATE_DEVICE_BGRA_SUPPORT;
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = E_FAIL;

    // ---- device first (no swap chain), so the modern per-HWND creation path can be used ----------
    if (!forceWarp) {
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr, flags, &fl, 1,
                               D3D11_SDK_VERSION, &dev_, nullptr, &ctx_);
    }
    if (forceWarp || FAILED(hr)) {
        // Plenty of classroom machines have no usable 3D driver: old integrated graphics with a
        // crippled/rolled-back driver, VMs, RDP sessions, machines where the GPU is claimed by another
        // process. WARP is the D3D11 software rasterizer and is always available; it costs CPU but
        // shows the lesson. Flagged in the client title bar so it is visible that rendering is on CPU.
        releaseD3D();
        fl = D3D_FEATURE_LEVEL_11_0;
        hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_WARP, nullptr, flags, &fl, 1,
                               D3D11_SDK_VERSION, &dev_, nullptr, &ctx_);
        warp_ = SUCCEEDED(hr);
    } else {
        warp_ = false;
    }
    if (FAILED(hr) || !dev_ || !ctx_) { releaseD3D(); return false; }

    // ---- swap chain: FLIP model first, legacy DISCARD as the fallback -----------------------------
    // WHY FLIP: with DXGI_SWAP_EFFECT_DISCARD the front buffer is undefined after every Present and the
    // OS has no composition surface to redraw from, so whenever Windows asks the window to repaint --
    // a popup menu closing over the video, un-minimising, alt-tab, taskbar preview -- that region came
    // back BLACK until the next Present. On a static desktop (differential mode) the next frame can be
    // 2 seconds away, which is exactly the reported "pick a right-click menu item and the screen
    // flickers / goes black". FLIP_DISCARD keeps the presented content composited by DWM, so those
    // repaints show the last frame instead of a hole. It also removes the extra copy and the tearing
    // risk of the bitblt model.
    bool made = false;
    {
        IDXGIDevice* dxgiDev = nullptr;
        IDXGIAdapter* adapter = nullptr;
        IDXGIFactory2* factory2 = nullptr;
        if (SUCCEEDED(dev_->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev)) && dxgiDev &&
            SUCCEEDED(dxgiDev->GetAdapter(&adapter)) && adapter &&
            SUCCEEDED(adapter->GetParent(__uuidof(IDXGIFactory2), (void**)&factory2)) && factory2) {
            DXGI_SWAP_CHAIN_DESC1 sd1 = {};
            sd1.Width = (UINT)w; sd1.Height = (UINT)h;
            sd1.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
            sd1.SampleDesc.Count = 1;
            sd1.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
            sd1.BufferCount = 3;                       // flip model: 2 is the minimum, 3 is smoother
            sd1.SwapEffect = DXGI_SWAP_EFFECT_FLIP_DISCARD;
            sd1.AlphaMode = DXGI_ALPHA_MODE_IGNORE;
            IDXGISwapChain1* sc1 = nullptr;
            hr = factory2->CreateSwapChainForHwnd(dev_, hwnd_, &sd1, nullptr, nullptr, &sc1);
            if (SUCCEEDED(hr) && sc1) {
                swap_ = sc1;   // IDXGISwapChain1 derives from IDXGISwapChain (upcast, refcount kept)
                made = true;
                flip_ = true;
            } else if (sc1) {
                sc1->Release();
            }
        }
        if (factory2) factory2->Release();
        if (adapter) adapter->Release();
        if (dxgiDev) dxgiDev->Release();
    }
    if (!made) {
        // Fallback for a machine/driver that refuses the flip model: the old legacy desc. Everything
        // above still applies -- paintEvent() re-presents, so even this path no longer shows holes.
        DXGI_SWAP_CHAIN_DESC sd = {};
        sd.BufferCount = 2;
        sd.BufferDesc.Width = (UINT)w; sd.BufferDesc.Height = (UINT)h;
        sd.BufferDesc.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
        sd.BufferDesc.RefreshRate = { 60, 1 };
        sd.BufferUsage = DXGI_USAGE_RENDER_TARGET_OUTPUT;
        sd.OutputWindow = hwnd_; sd.SampleDesc.Count = 1;
        sd.Windowed = TRUE; sd.SwapEffect = DXGI_SWAP_EFFECT_DISCARD;
        IDXGIDevice* dxgiDev = nullptr;
        IDXGIAdapter* adapter = nullptr;
        IDXGIFactory* factory = nullptr;
        if (SUCCEEDED(dev_->QueryInterface(__uuidof(IDXGIDevice), (void**)&dxgiDev)) && dxgiDev &&
            SUCCEEDED(dxgiDev->GetAdapter(&adapter)) && adapter &&
            SUCCEEDED(adapter->GetParent(__uuidof(IDXGIFactory), (void**)&factory)) && factory) {
            hr = factory->CreateSwapChain(dev_, &sd, &swap_);
        }
        if (factory) factory->Release();
        if (adapter) adapter->Release();
        if (dxgiDev) dxgiDev->Release();
        if (FAILED(hr) || !swap_) { releaseD3D(); return false; }
    }

    // force the next present() to re-derive everything size dependent from the real client rect:
    // after a device rebuild the cached clientW_/vidW_ no longer describe the new swap chain/texture,
    // and keeping them would make present() skip the resize and draw into a mismatched buffer.
    clientW_ = 0; clientH_ = 0; vidW_ = 0; vidH_ = 0;
    deviceLost_ = false;
    resizeFailStreak_ = 0;
    swap_->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer_);
    if (!backBuffer_ || FAILED(dev_->CreateRenderTargetView(backBuffer_, nullptr, &rtv_))) {
        releaseD3D();
        return false;
    }
    // state the model in stderr: FLIP keeps the presented content composited by DWM (no black holes
    // when popups/alt-tab invalidate the window), DISCARD is the legacy fallback for machines whose
    // driver refuses the flip path. Both are fixed for the menu-black by paintEvent(), but seeing which
    // one is active separates "driver refused flip" from "flip is broken here".
    fprintf(stderr, "[render] 交换链: %s%s\n",
            flip_ ? "FLIP_DISCARD 翻转模型" : "DISCARD 位块传送(回退)",
            warp_ ? " + WARP 软件光栅" : "");
    fflush(stderr);
    return true;
}

// generate a real system cursor texture: LoadCursor(IDC_ARROW) + GetIconInfo + DrawIconEx to render the standard
// arrow (with shadow/antialiasing, system rendering is 100% faithful), transparent pixels corrected with the AND mask
// -- replaces the old hand-drawn triangle (odd shape + no alpha, rendered as "white triangle + black block" with the
// opaque blend, not resembling a mouse at all).
// The texture top-left (0,0) is the pointer origin; the hotspot pixel offset is recorded in cursorHotX/Y and aligned
// to the pointer position when drawing.
bool D3D11Renderer::ensureCursorTexture() {
    if (cursorTex_ && cursorSrv_) return true;
    if (!dev_) return false;
    // Building the texture goes through GDI (LoadCursor/GetIconInfo/DrawIconEx). If that ever fails,
    // don't redo the whole GDI dance on every one of the ~50 presents per second: cap the retries.
    if (cursorTexTries_ >= 5) return false;
    ++cursorTexTries_;

    constexpr int kMax = 128;
    int w = 32, h = 32, hotX = 1, hotY = 1;   // fallback default: 32x32, hotspot (1,1)
    std::vector<uint8_t> px;

    HICON hIcon = LoadCursor(nullptr, IDC_ARROW);
    ICONINFO ii{};
    if (hIcon && GetIconInfo(hIcon, &ii)) {
        int iw = 0, ih = 0;
        BITMAP mb{};
        if (ii.hbmMask && GetObjectW(ii.hbmMask, sizeof(mb), &mb) && mb.bmWidth > 0) {
            iw = mb.bmWidth; ih = mb.bmHeight;
            if (ii.hbmColor) ih /= 2;   // color icon: mask height = 2x icon height (upper AND, lower XOR)
        }
        BITMAP cb{};
        if (ii.hbmColor && GetObjectW(ii.hbmColor, sizeof(cb), &cb) && cb.bmWidth > 0) {
            iw = cb.bmWidth; ih = cb.bmHeight;   // color bitmap size is authoritative
        }
        if (iw > 0 && ih > 0 && iw <= kMax && ih <= kMax) { w = iw; h = ih; }
        hotX = ii.xHotspot; hotY = ii.yHotspot;
        if (hotX < 0) hotX = 0; if (hotY < 0) hotY = 0;
        if (hotX >= w) hotX = w - 1; if (hotY >= h) hotY = h - 1;

        // read the AND mask (1bpp, upper half of mask): set bits = transparent
        std::vector<uint8_t> andMask((size_t)w * h, 0);
        bool haveAnd = false;
        if (ii.hbmMask) {
            BITMAPINFO bi{};
            bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
            bi.bmiHeader.biWidth = w;
            bi.bmiHeader.biHeight = -h;   // top-down, matches DIB row order
            bi.bmiHeader.biPlanes = 1;
            bi.bmiHeader.biBitCount = 1;
            bi.bmiHeader.biCompression = BI_RGB;
            HDC dc = GetDC(nullptr);
            if (dc) {
                haveAnd = GetDIBits(dc, ii.hbmMask, 0, h, andMask.data(), &bi, DIB_RGB_COLORS) != 0;
                ReleaseDC(nullptr, dc);
            }
        }

        // DrawIconEx renders the real cursor into a 32bpp DIB. For modern ARGB cursors (Win10/11 arrow)
        // the system rendering writes the real alpha channel (arrow body + antialiased/shadow edges) and the
        // background pixels come out alpha=0. The old code forced alpha 0 -> 255, which turned the transparent
        // background of the whole 32x32 bounding box opaque -> the visible "black square" around the cursor.
        // Verified with cursor_probe.exe: DrawIconEx output has alpha>0 on ~124/1024 px (arrow+shadow), the
        // remaining ~900 background px are alpha=0 and must stay transparent. Monochrome cursors (no hbmColor)
        // carry no alpha in the DIB, so those rely on the AND mask to define the shape and are forced opaque.
        const bool isColorCursor = (ii.hbmColor != nullptr);
        HDC dc = GetDC(nullptr);
        HDC mem = CreateCompatibleDC(dc);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;   // top-down
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        if (dib && bits) {
            HGDIOBJ old = SelectObject(mem, dib);
            memset(bits, 0, (size_t)w * h * 4);
            DrawIconEx(mem, 0, 0, hIcon, w, h, 0, nullptr, DI_NORMAL);
            GdiFlush();   // ensure the GDI queue is flushed before reading the memory
            const uint8_t* src = (const uint8_t*)bits;
            px.assign((size_t)w * h * 4, 0);
            for (int i = 0; i < w * h; ++i) {
                if (haveAnd && andMask[i]) continue;   // AND mask set -> fully transparent
                uint8_t r = src[i * 4 + 2], g = src[i * 4 + 1], b = src[i * 4 + 0];
                uint8_t a = src[i * 4 + 3];
                // color cursor: keep the real alpha (transparent background stays transparent, no black box);
                // monochrome cursor: DrawIconEx leaves alpha 0 -> AND mask is the only shape info, force opaque
                if (!isColorCursor && a == 0) a = 255;
                if (a == 0) continue;   // ARGB cursor transparent background: skip entirely
                px[i * 4 + 0] = b; px[i * 4 + 1] = g; px[i * 4 + 2] = r; px[i * 4 + 3] = a;
            }
            SelectObject(mem, old);
        }
        if (dib) DeleteObject(dib);
        if (mem) DeleteDC(mem);
        ReleaseDC(nullptr, dc);
    }
    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask)  DeleteObject(ii.hbmMask);

    if (px.empty()) return false;   // couldn't get a real cursor (theoretically impossible), skip this frame's overlay

    D3D11_TEXTURE2D_DESC td = {};
    td.Width = (UINT)w; td.Height = (UINT)h; td.MipLevels = td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_IMMUTABLE;
    td.BindFlags = D3D11_BIND_SHADER_RESOURCE;
    D3D11_SUBRESOURCE_DATA sd = {};
    sd.pSysMem = px.data(); sd.SysMemPitch = w * 4;
    if (FAILED(dev_->CreateTexture2D(&td, &sd, &cursorTex_))) return false;
    if (FAILED(dev_->CreateShaderResourceView(cursorTex_, nullptr, &cursorSrv_))) {
        cursorTex_->Release(); cursorTex_ = nullptr; return false;
    }
    cursorTexW_ = w; cursorTexH_ = h;
    cursorHotX_ = hotX; cursorHotY_ = hotY;
    return true;
}

// alpha blend state (standard non-premultiplied: source alpha / 1-source alpha). cursor transparent pixels are no longer rendered as black blocks.
bool D3D11Renderer::ensureBlendState() {
    if (blendAlpha_) return true;
    if (!dev_) return false;
    D3D11_BLEND_DESC bd{};
    bd.RenderTarget[0].BlendEnable = TRUE;
    bd.RenderTarget[0].SrcBlend = D3D11_BLEND_SRC_ALPHA;
    bd.RenderTarget[0].DestBlend = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOp = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].SrcBlendAlpha = D3D11_BLEND_ONE;
    bd.RenderTarget[0].DestBlendAlpha = D3D11_BLEND_INV_SRC_ALPHA;
    bd.RenderTarget[0].BlendOpAlpha = D3D11_BLEND_OP_ADD;
    bd.RenderTarget[0].RenderTargetWriteMask = D3D11_COLOR_WRITE_ENABLE_ALL;
    return SUCCEEDED(dev_->CreateBlendState(&bd, &blendAlpha_));
}

// draw the teacher's pointer on the topmost layer: map normalized coords to current picture pixels, cursor
// graphic aligns its hotspot to the picture coords.
void D3D11Renderer::drawCursorOverlay(float es, float scx, float scy, float halfVW, float halfVH) {
    cursorWinVisible_ = false;   // reset each frame; set true below only when actually drawn in view
    if (noSignal_) return;       // "waiting for teacher" placeholder: no remote pointer on it (keeps the local cursor visible)
    if (!haveCursor_ || !cursorVisible_ || !cursorSrv_ || !vs_ || !ps_ || !layout_ || !sampler_) return;
    if (vidW_ <= 0 || vidH_ <= 0 || cursorTexW_ <= 0) return;

    // normalized coords -> pixels inside the picture (scx/scy/halfVW are in picture pixels)
    float imgX = cursorNX_ * (float)vidW_;
    float imgY = cursorNY_ * (float)vidH_;
    // picture pixels -> window pixels: the picture center (scx,scy) corresponds to the window center
    float px = (float)clientW_ * 0.5f + (imgX - scx) * es;
    float py = (float)clientH_ * 0.5f + (imgY - scy) * es;

    // cursor display size: keep ~30 screen pixels (doesn't scale with zoom, stays discernable)
    const float size = 30.0f;
    float scale = size / (float)cursorTexW_;
    // hotspot alignment: the hotspot pixel (pointer tip) inside the texture maps to the teacher's pointer
    // position, the whole graphic shifts by the hotspot offset
    float left   = px - cursorHotX_ * scale;
    float top    = py - cursorHotY_ * scale;
    float right  = left + cursorTexW_ * scale;
    float bottom = top  + cursorTexH_ * scale;

    // skip entirely if it's outside the window
    if (right < 0 || left > (float)clientW_ || bottom < 0 || top > (float)clientH_) return;
    cursorWinVisible_ = true;   // the remote pointer is actually on screen (informational; the local cursor is never hidden)

    if (!cursorVB_) {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = sizeof(Vertex) * 6;
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(dev_->CreateBuffer(&bd, nullptr, &cursorVB_))) return;
    }

    float x0 = left / (float)clientW_ * 2.0f - 1.0f;
    float x1 = right / (float)clientW_ * 2.0f - 1.0f;
    float y1ndc = 1.0f - top / (float)clientH_ * 2.0f;
    float y0ndc = 1.0f - bottom / (float)clientH_ * 2.0f;
    Vertex verts[6] = {
        { x0, y1ndc, 0.0f, 0.0f }, { x1, y1ndc, 1.0f, 0.0f }, { x0, y0ndc, 0.0f, 1.0f },
        { x0, y0ndc, 0.0f, 1.0f }, { x1, y1ndc, 1.0f, 0.0f }, { x1, y0ndc, 1.0f, 1.0f },
    };
    D3D11_MAPPED_SUBRESOURCE vm;
    if (FAILED(ctx_->Map(cursorVB_, 0, D3D11_MAP_WRITE_DISCARD, 0, &vm))) return;
    memcpy(vm.pData, verts, sizeof(verts));
    ctx_->Unmap(cursorVB_, 0);

    UINT stride = sizeof(Vertex), off = 0;
    ctx_->IASetVertexBuffers(0, 1, &cursorVB_, &stride, &off);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->VSSetShader(vs_, nullptr, 0);
    ctx_->PSSetShader(ps_, nullptr, 0);
    ctx_->PSSetShaderResources(0, 1, &cursorSrv_);
    ctx_->PSSetSamplers(0, 1, &sampler_);
    // alpha blending: transparent pixels show the underlying picture, no longer rendered as black blocks (if creation fails it degrades to opaque, still displays)
    if (ensureBlendState()) ctx_->OMSetBlendState(blendAlpha_, nullptr, 0xFFFFFFFF);
    ctx_->Draw(6, 0);
    ctx_->OMSetBlendState(nullptr, nullptr, 0xFFFFFFFF);
}

void D3D11Renderer::releaseD3D() {
    if (cursorVB_)  { cursorVB_->Release(); cursorVB_ = nullptr; }
    if (cursorSrv_) { cursorSrv_->Release(); cursorSrv_ = nullptr; }
    if (cursorTex_) { cursorTex_->Release(); cursorTex_ = nullptr; }
    if (blendAlpha_){ blendAlpha_->Release(); blendAlpha_ = nullptr; }
    if (vb_)       { vb_->Release(); vb_ = nullptr; }
    if (sampler_)  { sampler_->Release(); sampler_ = nullptr; }
    if (srv_)      { srv_->Release(); srv_ = nullptr; }
    if (videoTex_) { videoTex_->Release(); videoTex_ = nullptr; }
    if (rtv_)      { rtv_->Release(); rtv_ = nullptr; }
    if (backBuffer_){ backBuffer_->Release(); backBuffer_ = nullptr; }
    if (swap_)     { swap_->Release(); swap_ = nullptr; }
    if (ctx_)      { ctx_->Release(); ctx_ = nullptr; }
    if (dev_)      { dev_->Release(); dev_ = nullptr; }
    flip_ = false;   // the model flag describes the swap chain that just went away
    // Everything derived from the old device/swap chain must be re-derived, otherwise the next
    // createDeviceAndSwapChain() + present() would believe the textures are still valid.
    clientW_ = 0; clientH_ = 0;
    vidW_ = 0; vidH_ = 0;
    cursorTexW_ = 0; cursorTexH_ = 0;
    cursorTexTries_ = 0;
    shaderTries_ = 0;
    haveCursor_ = false;
    cursorWinVisible_ = false;
    dragging_ = false;
}

void D3D11Renderer::destroy() {
    releaseD3D();
    if (hwnd_) {
        if (ownsWindow_) { DestroyWindow(hwnd_); }
        hwnd_ = nullptr;
    }
}

// shader + draw helper (D3DCompile at runtime, compatible with all GPUs) -----------
void D3D11Renderer::initShaders() {
    // per-instance check: the shader resources are instance members, so a global "already done"
    // flag would leave a second D3D11Renderer instance with null vs_/ps_/layout_/sampler_ and
    // never render. Gate on this instance's own resources instead.
    if (vs_ && ps_ && layout_ && sampler_) return;
    // D3DCompile is a real compile: if it fails (d3dcompiler_47.dll missing, driver quirk) retrying it
    // on all ~50 presents per second would burn CPU for nothing.
    if (shaderTries_ >= 3) return;
    ++shaderTries_;
    const char* vs = "struct VSIn { float2 p:POSITION; float2 t:TEXCOORD0; };"
                     "struct VSOut { float4 p:SV_POSITION; float2 t:TEXCOORD0; };"
                     "VSOut main(VSIn i){ VSOut o; o.p=float4(i.p,0,1); o.t=i.t; return o; }";
    const char* ps = "Texture2D tex:register(t0); SamplerState smp:register(s0);"
                     "float4 main(float4 p:SV_POSITION,float2 t:TEXCOORD0):SV_Target{return tex.Sample(smp,t);}";
    ID3DBlob *vb=nullptr,*pb=nullptr,*ve=nullptr,*pe=nullptr;
    if (FAILED(D3DCompile(vs,strlen(vs),nullptr,nullptr,nullptr,"main","vs_4_0",0,0,&vb,&ve)) ||
        FAILED(D3DCompile(ps,strlen(ps),nullptr,nullptr,nullptr,"main","ps_4_0",0,0,&pb,&pe)) ||
        !vb || !pb) {
        if(vb)vb->Release();if(pb)pb->Release();if(ve)ve->Release();if(pe)pe->Release();
        return;
    }
    if (vs_) { vs_->Release(); vs_ = nullptr; }
    if (ps_) { ps_->Release(); ps_ = nullptr; }
    if (layout_) { layout_->Release(); layout_ = nullptr; }
    if (sampler_) { sampler_->Release(); sampler_ = nullptr; }
    HRESULT hrVS = dev_->CreateVertexShader(vb->GetBufferPointer(),vb->GetBufferSize(),nullptr,&vs_);
    HRESULT hrPS = dev_->CreatePixelShader(pb->GetBufferPointer(),pb->GetBufferSize(),nullptr,&ps_);
    D3D11_INPUT_ELEMENT_DESC ed[2]={
        {"POSITION",0,DXGI_FORMAT_R32G32_FLOAT,0,0,D3D11_INPUT_PER_VERTEX_DATA},
        {"TEXCOORD",0,DXGI_FORMAT_R32G32_FLOAT,0,8,D3D11_INPUT_PER_VERTEX_DATA},
    };
    HRESULT hrLy = dev_->CreateInputLayout(ed,2,vb->GetBufferPointer(),vb->GetBufferSize(),&layout_);
    vb->Release();pb->Release();if(ve)ve->Release();if(pe)pe->Release();
    if (FAILED(hrVS) || FAILED(hrPS) || FAILED(hrLy) || !vs_ || !ps_ || !layout_) return;

    D3D11_SAMPLER_DESC sd={};
    sd.Filter=D3D11_FILTER_MIN_MAG_MIP_LINEAR;
    sd.AddressU=sd.AddressV=sd.AddressW=D3D11_TEXTURE_ADDRESS_CLAMP;
    sd.MinLOD = -FLT_MAX; sd.MaxLOD = FLT_MAX; sd.MaxAnisotropy = 1;
    dev_->CreateSamplerState(&sd,&sampler_);
}

// present (main rendering) ---------------------------------------------------
void D3D11Renderer::present(const uint8_t* bgra, int imgW, int imgH) {
    if (!hwnd_ || !swap_ || !bgra) return;
    if (imgW <= 0 || imgH <= 0) return;   // guard: a zero-size frame would make the quad scale div-by-zero -> NaN
    // A corrupt/absurd frame size must never reach the texture-creation path: D3D11 with an oversized
    // description either fails or allocates hundreds of MB. Reject it and let the decoder re-sync.
    if (imgW > 8192 || imgH > 8192) return;

    initShaders();
    if (!vs_ || !ps_ || !layout_ || !sampler_) return;

    // resize
    RECT rc; GetClientRect(hwnd_, &rc);
    int cw = rc.right - rc.left, ch = rc.bottom - rc.top;
    if (cw > 0 && ch > 0 && (cw != clientW_ || ch != clientH_)) {
        // A render target bound to the context keeps a reference to the back buffer, and ResizeBuffers
        // fails with DXGI_ERROR_INVALID_CALL while any reference is still outstanding. The previous
        // frame's OMSetRenderTargets left exactly such a reference, so the old "release then resize"
        // could fail every single time -> the picture stayed stretched at the old buffer size forever
        // (the failure branch just forced a retry). Unbind + Flush first, as the API requires.
        ctx_->OMSetRenderTargets(0, nullptr, nullptr);
        ctx_->Flush();
        if (rtv_) { rtv_->Release(); rtv_ = nullptr; }
        if (backBuffer_) { backBuffer_->Release(); backBuffer_ = nullptr; }
        if (FAILED(swap_->ResizeBuffers(0, 0, 0, DXGI_FORMAT_UNKNOWN, 0)) ||
            FAILED(swap_->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer_)) ||
            FAILED(dev_->CreateRenderTargetView(backBuffer_, nullptr, &rtv_))) {
            clientW_ = 0; clientH_ = 0;   // force a retry next present(); rtv_/backBuffer_ are null now
            // 10 consecutive failures is not a transient hiccup: the device (or the swap chain) is
            // gone. Flag it so the owner rebuilds instead of retrying until the end of the lesson.
            if (++resizeFailStreak_ >= 10) { resizeFailStreak_ = 0; deviceLost_ = true; }
            return;
        }
        resizeFailStreak_ = 0;
        clientW_ = cw; clientH_ = ch;
    }
    if (clientW_ <= 0 || clientH_ <= 0) return;

    // viewport
    D3D11_VIEWPORT vp{}; vp.Width=(float)clientW_; vp.Height=(float)clientH_; vp.MaxDepth=1.0f;
    ctx_->RSSetViewports(1, &vp);

    // video texture
    if (!videoTex_ || vidW_ != imgW || vidH_ != imgH) {
        if (videoTex_) { videoTex_->Release(); videoTex_ = nullptr; }
        if (srv_) { srv_->Release(); srv_ = nullptr; }
        D3D11_TEXTURE2D_DESC td={};
        td.Width=(UINT)imgW; td.Height=(UINT)imgH; td.MipLevels=td.ArraySize=1;
        td.Format=DXGI_FORMAT_B8G8R8A8_UNORM; td.SampleDesc.Count=1;
        td.Usage=D3D11_USAGE_DYNAMIC; td.BindFlags=D3D11_BIND_SHADER_RESOURCE;
        td.CPUAccessFlags=D3D11_CPU_ACCESS_WRITE;
        if (FAILED(dev_->CreateTexture2D(&td, nullptr, &videoTex_))) { vidW_=0;vidH_=0;return; }
        if (FAILED(dev_->CreateShaderResourceView(videoTex_, nullptr, &srv_))) {
            videoTex_->Release(); videoTex_=nullptr; vidW_=0;vidH_=0; return;
        }
        vidW_=imgW; vidH_=imgH;
    }

    // upload
    D3D11_MAPPED_SUBRESOURCE ms;
    if (SUCCEEDED(ctx_->Map(videoTex_, 0, D3D11_MAP_WRITE_DISCARD, 0, &ms))) {
        const uint8_t* src = bgra; uint8_t* dst = (uint8_t*)ms.pData;
        int sstride = imgW * 4;
        for (int y = 0; y < imgH; ++y) {
            memcpy(dst, src, (size_t)sstride);
            src += sstride; dst += ms.RowPitch;
        }
        ctx_->Unmap(videoTex_, 0);
    }

    // render
    ctx_->OMSetRenderTargets(1, &rtv_, nullptr);
    // The "waiting for teacher" placeholder is a FLAT colour, so letterboxing it only paints two
    // darker bars and the screen reads as a solid rectangle floating in the middle of the window.
    // Match the clear colour to the placeholder so the whole client area becomes one calm surface.
    FLOAT clear[4] = { 0.05f, 0.05f, 0.06f, 1.0f };
    if (noSignal_) { clear[0] = 26.0f / 255.0f; clear[1] = 34.0f / 255.0f; clear[2] = 76.0f / 255.0f; }
    ctx_->ClearRenderTargetView(rtv_, clear);

    float vw=(float)imgW, vh=(float)imgH;
    float bs = fmin((double)clientW_ / vw, (double)clientH_ / vh);
    float es = zoom_ * bs;
    float halfVW = clientW_ * 0.5f / es;
    float halfVH = clientH_ * 0.5f / es;
    // visible-region center (image pixels); panX/panY are the drag offset in window pixels
    float scx = vw * 0.5f + panX_ / es;
    float scy = vh * 0.5f + panY_ / es;
    // clamp the visible-region center:
    //  - zoomed in (visible region smaller than the image): keep it inside the image edges
    //  - fit / zoomed out (image smaller than the visible region): the image may slide around in the
    //    window (drag reveals the background around it), but its center stays inside the window so the
    //    picture is never dragged fully out of view
    if (halfVW * 2.0f >= vw) { float lo = vw*0.5f - halfVW, hi = vw*0.5f + halfVW; scx = fmin(fmax(scx, lo), hi); }
    else { scx = fmin(fmax(scx, halfVW), vw - halfVW); }
    if (halfVH * 2.0f >= vh) { float lo = vh*0.5f - halfVH, hi = vh*0.5f + halfVH; scy = fmin(fmax(scy, lo), hi); }
    else { scy = fmin(fmax(scy, halfVH), vh - halfVH); }

    // overlap of the visible region with the actual image -> the exact portion of the picture on screen.
    // The old code clamped UVs and "shrunk the quad around the window center", which re-centered the
    // picture instead of sliding it and also shrank letterboxed images (latent distortion); computing the
    // image/window overlap directly places the picture at its true on-screen position in every zoom state.
    float r0 = fmax(0.0f, scx - halfVW), r1 = fmin(vw, scx + halfVW);   // image coords, horizontal overlap
    float s0 = fmax(0.0f, scy - halfVH), s1 = fmin(vh, scy + halfVH);   // image coords, vertical overlap
    float u0 = 0, u1 = 0, v0 = 0, v1 = 0, x0 = 0, x1 = 0, y0 = 0, y1 = 0;
    if (r1 > r0 && s1 > s0) {
        // map the image overlap to window pixels (image pixel p -> window px (p - (scx-halfVW)) * clientW_/(2*halfVW))
        float kx = (float)clientW_ / (2.0f * halfVW);
        float ky = (float)clientH_ / (2.0f * halfVH);
        float wx0 = (r0 - (scx - halfVW)) * kx;   // window px, left edge
        float wx1 = (r1 - (scx - halfVW)) * kx;   // window px, right edge
        float wy0 = (s0 - (scy - halfVH)) * ky;   // window px, top edge
        float wy1 = (s1 - (scy - halfVH)) * ky;   // window px, bottom edge
        u0 = r0 / vw; u1 = r1 / vw;
        v0 = s0 / vh; v1 = s1 / vh;
        x0 = wx0 / clientW_ * 2.0f - 1.0f;
        x1 = wx1 / clientW_ * 2.0f - 1.0f;
        y1 = 1.0f - wy0 / clientH_ * 2.0f;   // NDC y is up: window top -> +1
        y0 = 1.0f - wy1 / clientH_ * 2.0f;
    }

    Vertex verts[6] = {
        {x0,y1,u0,v0},{x1,y1,u1,v0},{x0,y0,u0,v1},
        {x0,y0,u0,v1},{x1,y1,u1,v0},{x1,y0,u1,v1},
    };
    // persistent vertex buffer (created once, then UpdateSubresource per frame),
    // avoids the per-frame CreateBuffer+Release GPU allocation churn of the old version (one source of
    // latency/stutter at high frame rates)
    if (!vb_) {
        D3D11_BUFFER_DESC bd = {};
        bd.ByteWidth = sizeof(verts);
        bd.Usage = D3D11_USAGE_DYNAMIC;
        bd.BindFlags = D3D11_BIND_VERTEX_BUFFER;
        bd.CPUAccessFlags = D3D11_CPU_ACCESS_WRITE;
        if (FAILED(dev_->CreateBuffer(&bd, nullptr, &vb_)) || !vb_) { swap_->Present(0, 0); return; }
    }
    D3D11_MAPPED_SUBRESOURCE vm;
    if (SUCCEEDED(ctx_->Map(vb_, 0, D3D11_MAP_WRITE_DISCARD, 0, &vm))) {
        memcpy(vm.pData, verts, sizeof(verts));
        ctx_->Unmap(vb_, 0);
    }

    UINT stride=sizeof(Vertex),off=0;
    ctx_->IASetInputLayout(layout_);
    ctx_->IASetVertexBuffers(0,1,&vb_,&stride,&off);
    ctx_->IASetPrimitiveTopology(D3D11_PRIMITIVE_TOPOLOGY_TRIANGLELIST);
    ctx_->VSSetShader(vs_,nullptr,0);
    ctx_->PSSetShader(ps_,nullptr,0);
    ctx_->PSSetShaderResources(0,1,&srv_);
    ctx_->PSSetSamplers(0,1,&sampler_);
    ctx_->Draw(6,0);

    // teacher pointer overlay (drawn last, on the topmost layer)
    if (ensureCursorTexture())
        drawCursorOverlay(es, scx, scy, halfVW, halfVH);

    // Present(0,0): don't block on vsync, an already-decoded frame is shown immediately -- lowest end-to-end latency
    // (30fps streamed content + width keeping, tearing is invisible to the eye)
    HRESULT hrP = swap_->Present(0, 0);
    if (hrP == DXGI_ERROR_DEVICE_REMOVED || hrP == DXGI_ERROR_DEVICE_RESET ||
        hrP == DXGI_ERROR_DRIVER_INTERNAL_ERROR) {
        // TDR (the GPU was reset because a command took too long), a driver restart, a GPU switch or a
        // sleep/resume leaves the device dead: every later call fails too, and the old code ignored the
        // HRESULT entirely, so the student kept a frozen picture for the rest of the lesson. Flag it so
        // the widget tears the device down and rebuilds it (falling back to WARP if need be).
        deviceLost_ = true;
    } else if (flip_ && SUCCEEDED(hrP)) {
        // FLIP model contract: Present() rotates the buffers, so the render target view we just drew
        // into no longer refers to the current back buffer. Without re-acquiring it, the next frame is
        // rendered into a buffer that is never displayed -- the picture freezes or the window goes
        // black. (The legacy DISCARD chain has a fixed buffer 0 and does not need this.)
        ctx_->OMSetRenderTargets(0, nullptr, nullptr);
        if (rtv_) { rtv_->Release(); rtv_ = nullptr; }
        if (backBuffer_) { backBuffer_->Release(); backBuffer_ = nullptr; }
        if (FAILED(swap_->GetBuffer(0, __uuidof(ID3D11Texture2D), (void**)&backBuffer_)) || !backBuffer_ ||
            FAILED(dev_->CreateRenderTargetView(backBuffer_, nullptr, &rtv_))) {
            deviceLost_ = true;   // this chain is unusable: let the widget rebuild the device
        }
    }
}
