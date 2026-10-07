// ui/preview.cpp -- preview buffer and drawing
#include "ui/preview.h"
#include <algorithm>
#include <cwchar>
#include <cstring>

void Preview_Update(PreviewBuffer& pb, const uint8_t* bgra, int sw, int sh, int stride,
                    const RECT* dirty, int dirtyCount) {
    const int kMaxW = 960;
    int dw = sw > kMaxW ? kMaxW : sw;
    int dh = dw > 0 ? (int)((int64_t)sh * dw / sw) : 0;
    if (dw <= 0 || dh <= 0 || !bgra) return;

    // This runs ON THE CAPTURE THREAD, i.e. inside the capture -> encode -> send critical path.
    // The old version paid for a full 960x600 nearest-neighbour rebuild on EVERY frame -- 576k
    // iterations plus a fresh 2.3MB vector (alloc + free) per frame -- no matter that a lecture
    // slide changes 2% of its pixels per second. On a 4-core classroom PC that is the difference
    // between holding 50fps and stuttering, for nothing more than a thumbnail in our own window.
    //
    // Three changes, in decreasing order of value:
    //   1. THROTTLE to ~25fps: the preview is a thumbnail; 25fps reads as perfectly smooth.
    //   2. REGION updates: when the broken-up dirty area is under 30% of the frame, rescale ONLY
    //      those rectangles straight into the persistent thumbnail (identical per-pixel sampling
    //      formula as the full path, so the result is pixel-identical for that region).
    //   3. NO PER-FRAME ALLOCATION: the thumbnail lives in pb.bgra and is updated in place; the
    //      scratch vector only grows when the frame size changes.
    const uint64_t kThrottleMs = 40;
    uint64_t now = GetTickCount64();
    if (pb.primed && now - pb.lastMs < kThrottleMs) return;

    long long dirtyArea = 0;
    for (int i = 0; i < dirtyCount && dirty; ++i) {
        long long w = (long long)dirty[i].right - dirty[i].left;
        long long h = (long long)dirty[i].bottom - dirty[i].top;
        if (w > 0 && h > 0) dirtyArea += w * h;
    }
    const long long frameArea = (long long)sw * sh;
    const bool full = !pb.primed || pb.w != dw || pb.h != dh ||
                      dirtyCount <= 0 || dirtyArea * 10 >= frameArea * 3;   // >=30% or unknown

    std::lock_guard<std::mutex> lk(pb.mtx);
    if (full) {
        pb.bgra.resize((size_t)dw * dh * 4);
        for (int y = 0; y < dh; ++y) {
            int sy = (int)((int64_t)y * sh / dh);
            const uint8_t* src = bgra + (size_t)sy * stride;
            uint8_t* dst = pb.bgra.data() + (size_t)y * dw * 4;
            for (int x = 0; x < dw; ++x) {
                int sx = (int)((int64_t)x * sw / dw);
                memcpy(dst + x * 4, src + (size_t)sx * 4, 4);
            }
        }
        pb.w = dw; pb.h = dh;
        pb.primed = true;
    } else if (pb.bgra.size() == (size_t)dw * dh * 4) {
        // region path: for each dirty rect scale only the thumb pixels it covers, sampling with the
        // same inverse mapping the full path uses (thumb (x,y) <- source (x*sw/dw, y*sh/dh))
        for (int i = 0; i < dirtyCount && dirty; ++i) {
            int x0 = (int)((int64_t)dirty[i].left   * dw / sw);
            int x1 = (int)((int64_t)dirty[i].right  * dw / sw) + 1;   // +1: right/bottom are exclusive
            int y0 = (int)((int64_t)dirty[i].top    * dh / sh);
            int y1 = (int)((int64_t)dirty[i].bottom * dh / sh) + 1;
            if (x0 < 0) x0 = 0;
            if (y0 < 0) y0 = 0;
            if (x1 > dw) x1 = dw;
            if (y1 > dh) y1 = dh;
            for (int y = y0; y < y1; ++y) {
                int sy = (int)((int64_t)y * sh / dh);
                const uint8_t* src = bgra + (size_t)sy * stride;
                uint8_t* dst = pb.bgra.data() + (size_t)y * dw * 4;
                for (int x = x0; x < x1; ++x) {
                    int sx = (int)((int64_t)x * sw / dw);
                    memcpy(dst + x * 4, src + (size_t)sx * 4, 4);
                }
            }
        }
    }

    // scale the changed regions to thumbnail coordinates (green-border overlay in Preview_Draw)
    double kx = (double)dw / sw, ky = (double)dh / sh;
    pb.dirty.clear();
    int n = dirtyCount > 0 ? dirtyCount : 0;
    for (int i = 0; i < n && dirty; ++i) {
        RECT r;
        r.left   = (int)(dirty[i].left   * kx); r.right  = (int)(dirty[i].right  * kx);
        r.top    = (int)(dirty[i].top    * ky); r.bottom = (int)(dirty[i].bottom * ky);
        pb.dirty.push_back(r);
    }

    pb.lastMs = now;
    pb.seq.fetch_add(1);
}

void Preview_Draw(PreviewBuffer& pb, HDC hdc, const RECT& rc) {
    std::vector<uint8_t> img;
    int iw = 0, ih = 0;
    std::vector<RECT> dr;
    bool showDirty = false;
    {
        std::lock_guard<std::mutex> lk(pb.mtx);
        img = pb.bgra; iw = pb.w; ih = pb.h; dr = pb.dirty;
        showDirty = pb.showDirty.load();
    }
    int W = rc.right - rc.left, H = rc.bottom - rc.top;
    if (!iw || !ih || W <= 0 || H <= 0) {
        SetBkMode(hdc, TRANSPARENT);
        SetTextColor(hdc, RGB(120, 126, 140));
        const wchar_t* t = L"等待画面…";
        TextOutW(hdc, rc.left + 16, rc.top + 16, t, (int)wcslen(t));
        return;
    }

    // keep aspect ratio, centered
    double s = std::min((double)W / iw, (double)H / ih);
    int dw = (int)(iw * s), dh = (int)(ih * s);
    int dx = rc.left + (W - dw) / 2, dy = rc.top + (H - dh) / 2;

    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = iw;
    bi.bmiHeader.biHeight = -ih;        // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    SetStretchBltMode(hdc, HALFTONE);
    SetBrushOrgEx(hdc, 0, 0, nullptr);
    StretchDIBits(hdc, dx, dy, dw, dh, 0, 0, iw, ih, img.data(), &bi, DIB_RGB_COLORS, SRCCOPY);

    // differential changed regions (green border)
    if (showDirty && !dr.empty()) {
        HPEN pen = CreatePen(PS_SOLID, 2, RGB(0, 220, 90));
        HGDIOBJ oldPen = SelectObject(hdc, pen);
        SelectObject(hdc, GetStockObject(NULL_BRUSH));
        for (auto& r : dr) {
            int x0 = dx + (int)(r.left   * s), y0 = dy + (int)(r.top    * s);
            int x1 = dx + (int)(r.right  * s), y1 = dy + (int)(r.bottom * s);
            if (x1 - x0 < 3) x1 = x0 + 3;   // minimum visible box
            if (y1 - y0 < 3) y1 = y0 + 3;
            Rectangle(hdc, x0, y0, x1, y1);
        }
        SelectObject(hdc, oldPen);
        DeleteObject(pen);
    }
}
