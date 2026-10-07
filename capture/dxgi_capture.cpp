// capture/dxgi_capture.cpp —— DXGI Desktop Duplication implementation
#include "capture/dxgi_capture.h"
#include <wrl/client.h>
#include <cstring>

using Microsoft::WRL::ComPtr;

static int64_t nowMs() { return (int64_t)GetTickCount64(); }

DXGICapture::~DXGICapture() { stop(); }

bool DXGICapture::start(int outputIndex, FrameCallback cb) {
    std::lock_guard<std::mutex> lock(mtx_);
    outputIndex_ = outputIndex;
    releaseResources();              // clear leftovers from the last time, prevent pointer overwrite leak
    cb_ = std::move(cb);
    if (!createDeviceAndDuplication(outputIndex)) {
        // DXGI failed to start (no physical monitor / remote desktop not enabled / head-mounted GPU)
        // immediately try GDI: most Windows even without a monitor, GetDC(nullptr) + BitBlt still gets the virtual desktop
        fprintf(stderr, "[capture] DXGI 初始化失败, 尝试 GDI 兜底...\n");
        if (createGdiResources()) {
            // GDI mode: fills the primary desktop (cannot enumerate multiple monitors, assume one full screen)
            outW_ = GetSystemMetrics(SM_CXSCREEN);
            outH_ = GetSystemMetrics(SM_CYSCREEN);
            outX_ = 0; outY_ = 0;
            outStride_ = outW_ * 4;
            if (outW_ <= 0 || outH_ <= 0) {
                fprintf(stderr, "[capture] GDI 也无屏幕尺寸, 捕获不可用\n");
                releaseResources();
                return false;
            }
            readbackBuf_.resize((size_t)outStride_ * outH_);
            gdiMode_.store(true);
            running_ = true;
            fprintf(stderr, "[capture] GDI 兜底成功: %dx%d\n", outW_, outH_);
            return true;
        }
        fprintf(stderr, "[capture] DXGI/GDI 均失败, 屏幕捕获不可用\n");
        return false;
    }
    running_ = true;
    return true;
}

void DXGICapture::stop() {
    std::lock_guard<std::mutex> lock(mtx_);
    running_ = false;
    // reset GDI mode: on a fresh start, if the environment is still black, the periodic probe will auto-switch again
    // (not clearing leaves resources destroyed while gdiMode_ stays true, pumpFrameGdiLocked always returns false -> capture deadlock)
    gdiMode_.store(false);
    firstFrameChecked_ = false;
    lastBlackCheckMs_ = 0;
    lastGdiHash_ = 0;   // a fresh start must not compare against the previous session's grab
    releaseResources();
}

bool DXGICapture::createDeviceAndDuplication(int outputIndex) {
    // 1) D3D11 device (must have BGRA support, otherwise DuplicateOutput fails)
    D3D_FEATURE_LEVEL fl = D3D_FEATURE_LEVEL_11_0;
    HRESULT hr = D3D11CreateDevice(nullptr, D3D_DRIVER_TYPE_HARDWARE, nullptr,
                                   D3D11_CREATE_DEVICE_BGRA_SUPPORT, nullptr, 0,
                                   D3D11_SDK_VERSION, &device_, &fl, &context_);
    if (FAILED(hr)) return false;

    // 2) enumerate the target output (ComPtr auto-releases; multi-GPU machines must traverse adapters, guarantee same GPU as the captured monitor)
    ComPtr<IDXGIFactory1> factory;
    ComPtr<IDXGIAdapter>  adapter;
    ComPtr<IDXGIOutput>   output;
    if (FAILED(CreateDXGIFactory1(__uuidof(IDXGIFactory1), (void**)&factory))) { releaseResources(); return false; }
    if (FAILED(factory->EnumAdapters(0, &adapter)))                             { releaseResources(); return false; }
    if (FAILED(adapter->EnumOutputs(outputIndex, &output)))                     { releaseResources(); return false; }

    DXGI_OUTPUT_DESC od{};
    output->GetDesc(&od);
    outW_ = od.DesktopCoordinates.right - od.DesktopCoordinates.left;
    outH_ = od.DesktopCoordinates.bottom - od.DesktopCoordinates.top;
    outX_ = od.DesktopCoordinates.left;
    outY_ = od.DesktopCoordinates.top;
    if (outW_ <= 0 || outH_ <= 0) { releaseResources(); return false; }
    fprintf(stderr, "[capture] 显示器%d: %dx%d @ (%d,%d) output=%ls\n",
            outputIndex, outW_, outH_, outX_, outY_, od.DeviceName);

    // 3) desktop duplication session
    ComPtr<IDXGIOutput1> output1;
    if (FAILED(output->QueryInterface(__uuidof(IDXGIOutput1), (void**)&output1))) { releaseResources(); return false; }
    if (FAILED(output1->DuplicateOutput(device_, &duplication_)))                  { releaseResources(); return false; }

    // 4) double-buffered staging textures (CPU read-only), pre-created once, reused every frame
    D3D11_TEXTURE2D_DESC td{};
    td.Width = outW_; td.Height = outH_;
    td.MipLevels = 1; td.ArraySize = 1;
    td.Format = DXGI_FORMAT_B8G8R8A8_UNORM;
    td.SampleDesc.Count = 1;
    td.Usage = D3D11_USAGE_STAGING;
    td.CPUAccessFlags = D3D11_CPU_ACCESS_READ;
    for (int i = 0; i < 2; ++i) {
        if (FAILED(device_->CreateTexture2D(&td, nullptr, &stagingTex_[i]))) { releaseResources(); return false; }
    }

    outStride_ = outW_ * 4;
    readbackBuf_.resize((size_t)outStride_ * outH_);
    return true;
}

void DXGICapture::releaseResources() {
    for (auto& t : stagingTex_) { if (t) { t->Release(); t = nullptr; } }
    if (duplication_) { duplication_->Release(); duplication_ = nullptr; }
    if (context_)     { context_->Release();     context_ = nullptr; }
    if (device_)      { device_->Release();      device_ = nullptr; }
    destroyGdiResources();
}

void DXGICapture::setExcludeScreenRect(const RECT& rc, bool enabled) {
    excludeEnabled_.store(enabled);
    std::lock_guard<std::mutex> lock(excludeMtx_);
    excludeRect_ = rc;
}

bool DXGICapture::pumpFrame() {
    std::lock_guard<std::mutex> lock(mtx_);
    if (!running_) return false;
    if (gdiMode_.load()) return pumpFrameGdiLocked();
    if (!duplication_) {
        // last rebuild failed (lock screen/session switch): auto-retry every 500ms, recover streaming once rebuilt (self-heal, no infinite loop)
        int64_t nowT = nowMs();
        if (nowT - lastRecreateMs_ < 500) return false;
        lastRecreateMs_ = nowT;
        if (!createDeviceAndDuplication(outputIndex_)) return false;
    }

    // bounded-timeout polling: when the picture doesn't change it returns WAIT_TIMEOUT, so the thread can respond to exit
    DXGI_OUTDUPL_FRAME_INFO info{};
    IDXGIResource* desktopRes = nullptr;
    HRESULT hr = duplication_->AcquireNextFrame(16, &info, &desktopRes);
    if (hr == DXGI_ERROR_WAIT_TIMEOUT) {
        // when the desktop is fully static DXGI stops producing frames -> the teacher is silent, newly joined
        // clients can never get a picture (black screen).
        int64_t now = nowMs();
        if (!firstPumpMs_) firstPumpMs_ = now;
        // consume a pending "emit now" request here, BEFORE the fallback decision below: a client that
        // joins while the capture path is still dead must not wait out the full 3s DXGI-probe window.
        bool force = forceEmit_.exchange(false);

        // A) DXGI HAS DELIVERED NOTHING AT ALL -> this is a dead end, not an idle desktop.
        //   Everything below is gated on `haveFrame_` (there is no buffer to synthesise a keep-alive from,
        //   and the black-screen probe also needs a read-back to compare against), so on a machine whose
        //   desktop duplication never yields a frame -- virtual display adapters, some remote sessions --
        //   the teacher stayed COMPLETELY silent forever and a student saw "waiting for teacher" for as long
        //   as they were willing to stare at it (measured: 12s+ on all three attempts). GDI can still see
        //   the desktop, so if what it sees is not black the failure is DXGI's: switch to the GDI path,
        //   which produces frames on a timer regardless of desktop activity.
        //   Normal case: give DXGI 3s to prove itself. Forced case (a client is waiting, see
        //   requestFrameSoon()): probe after only 0.8s -- an idle desktop wastes at most 0.8s of a
        //   joining student's time instead of 3s, and a healthy DXGI still wins the probe because it
        //   would have delivered its first frame long before that.
        if (!haveFrame_ && now - firstPumpMs_ >= (force ? 800 : 3000)) {
            int gdiAvg = gdiProbeBrightness();
            if (gdiAvg > 20) {
                gdiMode_.store(true);
                firstFrameChecked_ = false;
                lastBlackCheckMs_ = 0;
                lastGdiHash_ = 0;
                fprintf(stderr, "[capture] DXGI 长时间未产出任何帧(GDI 亮度=%d) -> 切换 GDI 捕获\n", gdiAvg);
                return false;   // the GDI path takes over from the next pump
            }
        }

        // 1) periodically probe for black (a static DXGI black screen is caught too; the old version only counted
        //    by frame and missed static desktops)
        // 2) simulate a full-screen change frame every 2s from the last frame buffer, keeping keyframes flowing
        //    (new clients can get a picture within <=2s) -- and IMMEDIATELY when a client has asked for one,
        //    because waiting out that interval is exactly the "the picture takes ages to appear" report
        if (haveFrame_ && now - lastBlackCheckMs_ >= 4000) {
            lastBlackCheckMs_ = now;
            if (detectBlackSwitch()) return false;   // already switched to GDI, next round emits from the GDI path
        }
        // `force` was consumed at the top of this branch (see above)
        if (haveFrame_ && (force || now - lastEmitMs_ >= 2000)) {
            lastEmitMs_ = now;
            CapturedFrame f;
            f.dirtyRects.push_back(RECT{ 0, 0, outW_, outH_ });   // full-screen dirty -> differential doesn't skip
            f.dirtyArea = (uint64_t)outW_ * outH_;
            f.data = readbackBuf_.data();
            f.width = outW_; f.height = outH_; f.stride = outStride_;
            f.ptsMs = (uint64_t)now;
            if (cb_) cb_(f);
            return true;
        }
        return false;
    }
    if (hr == DXGI_ERROR_ACCESS_LOST) {   // lock screen/resolution change/UAC popup: rebuild the whole chain
        releaseResources();
        lastRecreateMs_ = nowMs();
        createDeviceAndDuplication(outputIndex_);   // on failure the top of pumpFrame retries every 500ms
        return false;
    }
    if (FAILED(hr)) return false;

    // AcquireNextFrame returns an IDXGIResource*, convert to a D3D11 texture then release the reference
    ID3D11Texture2D* desktopImage = nullptr;
    desktopRes->QueryInterface(__uuidof(ID3D11Texture2D), (void**)&desktopImage);
    desktopRes->Release();
    if (!desktopImage) return false;

    // copy to staging (double-buffer rotation)
    ID3D11Texture2D* staging = stagingTex_[stagingIdx_];
    stagingIdx_ = 1 - stagingIdx_;
    context_->CopyResource(staging, desktopImage);

    // the changed-region metadata must be read before ReleaseFrame (first frame TotalMetadataBufferSize=0, treated as full screen)
    CapturedFrame f;
    f.dirtyRects.clear();
    f.dirtyArea = 0;
    // changed-region metadata: move and dirty each use their own buffer (MSDN official sample approach)
    // note: allocate buffer size per each one's need, with coordinate validity checks against garbage data
    UINT metaSize = info.TotalMetadataBufferSize;
    if (metaSize > 0) {
        moveBuf_.resize(metaSize);
        dirtyBuf_.resize(metaSize);
        UINT moveCount = 0;
        DXGI_OUTDUPL_MOVE_RECT* moves = (DXGI_OUTDUPL_MOVE_RECT*)moveBuf_.data();
        if (SUCCEEDED(duplication_->GetFrameMoveRects(metaSize, moves, &moveCount))) {
            // key: GetFrameMoveRects' third out-param is "required buffer size (bytes)", NOT the rect count!
            // using it directly as a loop count reads past by moveCount*24 bytes (up to ~24x) per 24B element,
            // one of the historical "occasional 0xC0000005 can't reproduce" root causes (doesn't crash by luck when within the heap).
            // actual rect count = byte count / sizeof(DXGI_OUTDUPL_MOVE_RECT).
            UINT moveRects = moveCount / (UINT)sizeof(DXGI_OUTDUPL_MOVE_RECT);
            for (UINT i = 0; i < moveRects; ++i) {
                RECT r = moves[i].DestinationRect;
                if (r.left < 0 || r.top < 0 || r.right > outW_ || r.bottom > outH_ ||
                    r.right <= r.left || r.bottom <= r.top) continue;   // guard against garbage data
                f.dirtyRects.push_back(r);
                f.dirtyArea += (uint64_t)(r.right - r.left) * (uint64_t)(r.bottom - r.top);
            }
        }
        UINT dirtyCount = 0;
        RECT* dirty = (RECT*)dirtyBuf_.data();
        if (SUCCEEDED(duplication_->GetFrameDirtyRects(metaSize, dirty, &dirtyCount))) {
            // same: dirtyCount is "required buffer size (bytes)", divide by sizeof(RECT) for the rect count.
            // old code looped by byte count -> read past dirtyCount*16 bytes (historical occasional crash root cause)
            UINT dirtyRects = dirtyCount / (UINT)sizeof(RECT);
            for (UINT i = 0; i < dirtyRects; ++i) {
                RECT r = dirty[i];
                if (r.left < 0 || r.top < 0 || r.right > outW_ || r.bottom > outH_ ||
                    r.right <= r.left || r.bottom <= r.top) continue;   // guard against garbage data
                f.dirtyRects.push_back(r);
                f.dirtyArea += (uint64_t)(r.right - r.left) * (uint64_t)(r.bottom - r.top);
            }
        }
        if (f.dirtyArea == 0) {   // metadata anomaly fallback: treat as full screen, better to over-send than lose the picture
            f.dirtyRects.push_back(RECT{ 0, 0, outW_, outH_ });
            f.dirtyArea = (uint64_t)outW_ * outH_;
        }
    } else {
        f.dirtyRects.push_back(RECT{ 0, 0, outW_, outH_ });
        f.dirtyArea = (uint64_t)outW_ * outH_;
    }
    if (f.dirtyRects.size() > 64) f.dirtyRects.resize(64);   // preview only draws the first 64
    duplication_->ReleaseFrame();

    // Map read-back (copy row by row per RowPitch, guarding against row-align padding)
    D3D11_MAPPED_SUBRESOURCE mapped{};
    if (FAILED(context_->Map(staging, 0, D3D11_MAP_READ, 0, &mapped))) return false;
    const uint8_t* src = (const uint8_t*)mapped.pData;
    for (int y = 0; y < outH_; ++y)
        memcpy(readbackBuf_.data() + (size_t)y * outStride_,
               src + (size_t)y * mapped.RowPitch, outStride_);
    context_->Unmap(staging, 0);

    // black-screen detection (DXGI captures black under remote/virtual display): probe the first frame immediately
    // (judge black-on-open instantly), then every ~4s timer probe (timer-based, not frame-rate dependent, catches static black too);
    // if GDI brightness is clearly higher, decide DXGI black, auto-switch to GDI fallback (runtime DXGI sudden-black self-heals)
    if (!gdiMode_.load()) {
        int64_t nowT = nowMs();
        bool first = !firstFrameChecked_;
        if (first) firstFrameChecked_ = true;
        if (first || nowT - lastBlackCheckMs_ >= 4000) {
            lastBlackCheckMs_ = nowT;
            detectBlackSwitch();
        }
    }

    // exclude the teacher's own window region: fill with a fixed color and deduct its dirty rects,
    // 1) preview/client picture no longer contains this window -> eliminates "picture-in-picture" recursive flicker
    // 2) this window's redraws no longer count toward dirty -> a static desktop can truly trigger differential skip
    applyExclude(readbackBuf_.data(), f, outW_, outH_, outStride_);

    f.data = readbackBuf_.data();
    f.width = outW_; f.height = outH_; f.stride = outStride_;
    f.ptsMs = (uint64_t)nowMs();
    lastEmitMs_ = (int64_t)f.ptsMs;
    haveFrame_ = true;
    if (cb_) cb_(f);   // note: the callback runs while holding the lock, calling this class's methods inside is forbidden (deadlock)
    return true;
}

// ============ GDI fallback (DXGI captures black under remote/virtual display) ============

bool DXGICapture::createGdiResources() {
    if (gdiMemDC_) return true;
    gdiScreenDC_ = GetDC(nullptr);
    if (!gdiScreenDC_) return false;
    gdiMemDC_ = CreateCompatibleDC(gdiScreenDC_);
    if (!gdiMemDC_) { ReleaseDC(nullptr, gdiScreenDC_); gdiScreenDC_ = nullptr; return false; }
    memset(&gdiBmi_, 0, sizeof(gdiBmi_));
    gdiBmi_.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    gdiBmi_.bmiHeader.biWidth = outW_;
    gdiBmi_.bmiHeader.biHeight = -outH_;          // negative height = top-down, consistent with DXGI read-back
    gdiBmi_.bmiHeader.biPlanes = 1;
    gdiBmi_.bmiHeader.biBitCount = 32;
    gdiBmi_.bmiHeader.biCompression = BI_RGB;     // 32bpp = BGRX, i.e. BGRA
    gdiDib_ = CreateDIBSection(gdiScreenDC_, &gdiBmi_, DIB_RGB_COLORS, (void**)&gdiBits_, nullptr, 0);
    if (!gdiDib_) { destroyGdiResources(); return false; }
    gdiOld_ = SelectObject(gdiMemDC_, gdiDib_);
    return true;
}

void DXGICapture::destroyGdiResources() {
    if (gdiMemDC_ && gdiOld_) SelectObject(gdiMemDC_, gdiOld_);
    gdiOld_ = nullptr;
    if (gdiDib_) { DeleteObject(gdiDib_); gdiDib_ = nullptr; }
    if (gdiMemDC_) { DeleteDC(gdiMemDC_); gdiMemDC_ = nullptr; }
    if (gdiScreenDC_) { ReleaseDC(nullptr, gdiScreenDC_); gdiScreenDC_ = nullptr; }
    gdiBits_ = nullptr;
}

// grab the screen once and return the average brightness (for black comparison)
int DXGICapture::gdiProbeBrightness() {
    bool own = !gdiMemDC_;
    if (own && !createGdiResources()) return -1;
    BitBlt(gdiMemDC_, 0, 0, outW_, outH_, gdiScreenDC_, outX_, outY_, SRCCOPY);
    int avg = -1;
    if (gdiBits_)
        avg = sampleBrightness(gdiBits_, outW_ * 4, outW_, outH_);
    if (own) destroyGdiResources();
    return avg;
}

// sparse-grid sampled average brightness (sample every 48px, negligible cost)
int DXGICapture::sampleBrightness(const uint8_t* data, int stride, int w, int h) {
    if (!data || w <= 0 || h <= 0 || stride < w * 4) return -1;
    int64_t sum = 0; int cnt = 0;
    for (int y = 0; y < h; y += 48)
        for (int x = 0; x < w; x += 48) {
            const uint8_t* q = data + (size_t)y * stride + (size_t)x * 4;
            sum += q[0] + q[1] + q[2]; cnt++;
        }
    return cnt ? (int)(sum / cnt) : -1;
}

// DXGI black-screen probe: if the GDI live-shot brightness is clearly higher than the DXGI read-back (DXGI all-black), judge black and switch to GDI fallback
bool DXGICapture::detectBlackSwitch() {
    if (gdiMode_.load() || !duplication_ || outW_ <= 0 || outH_ <= 0) return false;
    if (readbackBuf_.size() < (size_t)outStride_ * outH_) return false;
    int dxgiAvg = sampleBrightness(readbackBuf_.data(), outStride_, outW_, outH_);
    if (dxgiAvg < 0) return false;
    int gdiAvg = gdiProbeBrightness();
    if (gdiAvg < 0) return false;
    if (gdiAvg > dxgiAvg + 60) {   // GDI clearly brighter -> DXGI capture anomaly (black)
        createGdiResources();
        gdiMode_.store(true);
        fprintf(stderr, "[capture] DXGI 捕获亮度异常(DXGI avg=%d, GDI avg=%d), 已切换 GDI 捕获模式\n",
                dxgiAvg, gdiAvg);
        return true;
    }
    return false;
}

// Sparse sample-grid hash used as a change detector in GDI mode. Step 8px: anything larger than a
// character cell is caught, while the cost on a 2560x1600 screen is a few hundred microseconds.
// FNV-1a over the 24 significant bits of each sample (alpha is constant, skipping it is cheaper).
uint64_t DXGICapture::gdiFrameHash(const uint8_t* data, int w, int h, int stride) {
    if (!data || w <= 0 || h <= 0 || stride < w * 4) return 0;
    uint64_t hash = 1469598103934665603ULL;      // FNV-1a offset basis
    for (int y = 0; y < h; y += 8) {
        const uint8_t* row = data + (size_t)y * stride;
        for (int x = 0; x < w; x += 8) {
            const uint8_t* p = row + (size_t)x * 4;
            hash ^= (uint64_t)(p[0] | (p[1] << 8) | (p[2] << 16));
            hash *= 1099511628211ULL;            // FNV prime
        }
    }
    return hash;
}

// GDI mode frame grab: BitBlt + direct DIB memory read (BGRA), full-screen change, capped at 30fps to control bandwidth
bool DXGICapture::pumpFrameGdiLocked() {
    if (!gdiMemDC_ || !gdiBits_) return false;
    int64_t now = nowMs();
    // 33ms = 30fps. Was 66ms (~15fps), which silently halved the frame rate on any machine that had
    // fallen back to GDI (remote desktop / virtual display) -- the picture looked choppy no matter
    // what the quality preset said. GDI has no dirty-rect metadata so every grab is a full-screen
    // BitBlt; 30fps of that is still cheap on a modern CPU.
    if (now - lastGdiEmitMs_ < 33) return false;
    if (!BitBlt(gdiMemDC_, 0, 0, outW_, outH_, gdiScreenDC_, outX_, outY_, SRCCOPY)) return false;
    lastGdiEmitMs_ = now;

    // GDI has no dirty-rect metadata, so every grab used to look like "the whole screen changed".
    // That killed differential mode precisely in the environment that needs it most: GDI is the
    // only working capture path under remote desktop / virtual display, and there a static desktop
    // kept encoding 30 frames/s at full bitrate. Compare a sparse sample grid with the previous
    // grab: when nothing moved, report an EMPTY dirty set and let the streamer's differential check
    // skip the frame. The streamer's 2s keep-alive still emits a frame now and then so clients that
    // join later get a picture.
    const uint64_t frameHash = gdiFrameHash(gdiBits_, outW_, outH_, outW_ * 4);
    const bool unchanged = haveFrame_ && frameHash == lastGdiHash_;
    lastGdiHash_ = frameHash;

    CapturedFrame f;
    if (unchanged) {
        f.dirtyArea = 0;                                       // no dirtyRects -> differential sees 0%
    } else {
        f.dirtyRects.push_back(RECT{ 0, 0, outW_, outH_ });    // GDI has no dirty info, full screen
        f.dirtyArea = (uint64_t)outW_ * outH_;
    }
    applyExclude(gdiBits_, f, outW_, outH_, outW_ * 4);
    f.data = gdiBits_;
    f.width = outW_; f.height = outH_; f.stride = outW_ * 4;
    f.ptsMs = (uint64_t)now;
    lastEmitMs_ = now;
    haveFrame_ = true;
    if (cb_) cb_(f);
    return true;
}

// excluded-region fill + dirty deduction (shared logic, used by both DXGI/GDI paths)
void DXGICapture::applyExclude(uint8_t* data, CapturedFrame& f, int w, int h, int stride) {
    if (!excludeEnabled_.load()) return;
    RECT er;
    { std::lock_guard<std::mutex> lock(excludeMtx_); er = excludeRect_; }
    er.left -= outX_; er.top -= outY_; er.right -= outX_; er.bottom -= outY_;
    int L = (int)er.left   > 0 ? (int)er.left   : 0;
    int T = (int)er.top    > 0 ? (int)er.top    : 0;
    int R = (int)er.right  < w  ? (int)er.right  : w;
    int B = (int)er.bottom < h  ? (int)er.bottom : h;
    if (R > L && B > T) {
        // Fill colour = EXACTLY the teacher preview CARD's own background (QSS `QWidget#previewFrame`
        // = #121826). The excluded rect is the preview WIDGET, which sits inside that card behind an
        // 8px card-coloured margin, so filling with the card colour makes the area blend into the
        // panel instead of reading as a rectangle pasted onto the picture.
        // The previous value was #34221E -- a murky red-brown, because the B/G/R channels had been
        // written in the wrong order. It clashed hard with the blue-grey UI and was reported on the
        // student screen as "a patch blocking the picture".
        constexpr uint8_t kFillB = 0x26, kFillG = 0x18, kFillR = 0x12;   // BGRA -> #121826
        for (int y = T; y < B; ++y) {
            uint8_t* p = data + (size_t)y * stride + (size_t)L * 4;
            for (int x = L; x < R; ++x) {
                p[0] = kFillB; p[1] = kFillG; p[2] = kFillR; p[3] = 255; p += 4;
            }
        }
        std::vector<RECT> keep;
        uint64_t area = 0;
        for (const RECT& r : f.dirtyRects) {
            bool inside = r.left >= L && r.top >= T && r.right <= R && r.bottom <= B;
            if (inside) continue;
            area += (uint64_t)(r.right - r.left) * (r.bottom - r.top);
            int il = (int)r.left   > L ? (int)r.left   : L;
            int it = (int)r.top    > T ? (int)r.top    : T;
            int ir = (int)r.right  < R ? (int)r.right  : R;
            int ib = (int)r.bottom < B ? (int)r.bottom : B;
            if (ir > il && ib > it) area -= (uint64_t)(ir - il) * (ib - it);
            keep.push_back(r);
        }
        f.dirtyRects = std::move(keep);
        f.dirtyArea = area;
    }
}
