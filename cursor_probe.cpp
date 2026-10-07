// cursor_probe.cpp -- empirically probe how the system arrow cursor's pixels come out:
//   1) does GetDIBits(hbmColor) return a valid alpha channel? (ARGB cursor -> yes)
//   2) does DrawIconEx into a zeroed DIB write alpha? (or leave alpha=0 -> old black-square bug)
//   3) what does the AND mask look like for the modern arrow? (all-zero for ARGB cursors)
// Build: cl /nologo /O2 /EHsc cursor_probe.cpp user32.lib gdi32.lib
#include <windows.h>
#include <cstdio>
#include <vector>
#include <algorithm>

struct Probe {
    int w = 0, h = 0;
    int hotX = 0, hotY = 0;
    int nPx = 0;
    int nAlphaNonzero = 0;      // pixels with alpha != 0
    int nAlpha255 = 0;          // pixels with alpha == 255
    int nAlphaLow = 0;          // alpha in (0,255) - anti-aliased/shadow edge
    int nAndMaskSet = 0;        // AND-mask transparent bits
    int nColorBlack = 0;        // RGB==0 pixels
    std::vector<uint8_t> andMask;
    std::vector<uint8_t> dibBits;   // from GetDIBits(hbmColor)
    std::vector<uint8_t> iconBits;  // from DrawIconEx into zeroed DIB
};

static bool readColorViaGetDIBits(HBITMAP hbm, int w, int h, std::vector<uint8_t>& out) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;   // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    out.assign((size_t)w * h * 4, 0);
    HDC dc = GetDC(nullptr);
    bool ok = dc && GetDIBits(dc, hbm, 0, h, out.data(), &bi, DIB_RGB_COLORS) != 0;
    if (dc) ReleaseDC(nullptr, dc);
    return ok;
}

static bool readAndMask(HBITMAP hbmMask, int w, int h, std::vector<uint8_t>& out) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 1;
    bi.bmiHeader.biCompression = BI_RGB;
    out.assign((size_t)w * h, 0);
    HDC dc = GetDC(nullptr);
    bool ok = dc && GetDIBits(dc, hbmMask, 0, h, out.data(), &bi, DIB_RGB_COLORS) != 0;
    if (dc) ReleaseDC(nullptr, dc);
    return ok;
}

static bool drawIconEx(HICON hIcon, int w, int h, std::vector<uint8_t>& out) {
    HDC dc = GetDC(nullptr);
    HDC mem = CreateCompatibleDC(dc);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(dc, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    bool ok = false;
    if (dib && bits) {
        HGDIOBJ old = SelectObject(mem, dib);
        memset(bits, 0, (size_t)w * h * 4);
        DrawIconEx(mem, 0, 0, hIcon, w, h, 0, nullptr, DI_NORMAL);
        GdiFlush();
        out.assign((size_t)w * h * 4, 0);
        memcpy(out.data(), bits, (size_t)w * h * 4);
        SelectObject(mem, old);
        ok = true;
    }
    if (dib) DeleteObject(dib);
    if (mem) DeleteDC(mem);
    ReleaseDC(nullptr, dc);
    return ok;
}

static void analyze(const char* name, const std::vector<uint8_t>& px, Probe& p) {
    p.nPx = 0; p.nAlphaNonzero = 0; p.nAlpha255 = 0; p.nAlphaLow = 0; p.nColorBlack = 0;
    size_t n = px.size() / 4;
    for (size_t i = 0; i < n; ++i) {
        uint8_t a = px[i * 4 + 3];
        uint8_t b = px[i * 4 + 0], g = px[i * 4 + 1], r = px[i * 4 + 2];
        p.nPx++;
        if (a != 0) p.nAlphaNonzero++;
        if (a == 255) p.nAlpha255++;
        if (a > 0 && a < 255) p.nAlphaLow++;
        if (r == 0 && g == 0 && b == 0) p.nColorBlack++;
    }
    printf("%-22s total=%d alpha>0=%d alpha==255=%d alpha(0,255)=%d rgbBlack=%d\n",
           name, p.nPx, p.nAlphaNonzero, p.nAlpha255, p.nAlphaLow, p.nColorBlack);
}

int main() {
    printf("=== cursor_probe: system arrow (IDC_ARROW) alpha behavior ===\n");
    HICON hIcon = LoadCursor(nullptr, IDC_ARROW);
    if (!hIcon) { printf("LoadCursor failed\n"); return 1; }
    ICONINFO ii{};
    if (!GetIconInfo(hIcon, &ii)) { printf("GetIconInfo failed\n"); return 1; }

    int w = 0, h = 0;
    BITMAP mb{};
    if (ii.hbmMask && GetObjectW(ii.hbmMask, sizeof(mb), &mb) && mb.bmWidth > 0) {
        w = mb.bmWidth; h = mb.bmHeight;
        if (ii.hbmColor) h /= 2;   // color icon: mask height = 2x (upper AND, lower XOR)
    }
    BITMAP cb{};
    if (ii.hbmColor && GetObjectW(ii.hbmColor, sizeof(cb), &cb) && cb.bmWidth > 0) {
        w = cb.bmWidth; h = cb.bmHeight;
    }
    printf("hbmColor=%p hbmMask=%p size=%dx%d hotspot=(%d,%d)\n",
           (void*)ii.hbmColor, (void*)ii.hbmMask, w, h, ii.xHotspot, ii.yHotspot);

    Probe p;
    p.w = w; p.h = h; p.hotX = ii.xHotspot; p.hotY = ii.yHotspot;

    // 1) AND mask
    if (ii.hbmMask && readAndMask(ii.hbmMask, w, h, p.andMask)) {
        p.nAndMaskSet = 0;
        for (int i = 0; i < w * h; ++i) if (p.andMask[i]) p.nAndMaskSet++;
        printf("AND mask: %d/%d bits set (transparent)\n", p.nAndMaskSet, w * h);
    } else {
        printf("AND mask: read failed\n");
    }

    // 2) color bitmap via GetDIBits
    if (ii.hbmColor) {
        bool ok = readColorViaGetDIBits(ii.hbmColor, w, h, p.dibBits);
        printf("GetDIBits(hbmColor) -> %s\n", ok ? "ok" : "FAILED");
        if (ok) analyze("GetDIBits(hbmColor)", p.dibBits, p);
    } else {
        printf("hbmColor == NULL (monochrome cursor)\n");
    }

    // 3) DrawIconEx into zeroed DIB
    bool ok2 = drawIconEx(hIcon, w, h, p.iconBits);
    printf("DrawIconEx -> %s\n", ok2 ? "ok" : "FAILED");
    if (ok2) analyze("DrawIconEx", p.iconBits, p);

    // 4) overlap: in DrawIconEx output, pixels where AND mask set -- do they have alpha 0 or 255?
    if (ok2 && !p.andMask.empty()) {
        int andSetAlpha255 = 0, andSetAlpha0 = 0;
        int andClearAlpha0 = 0, andClearAlpha255 = 0;
        for (int i = 0; i < w * h; ++i) {
            uint8_t a = p.iconBits[i * 4 + 3];
            if (p.andMask[i]) { if (a == 255) andSetAlpha255++; else if (a == 0) andSetAlpha0++; }
            else { if (a == 0) andClearAlpha0++; else if (a == 255) andClearAlpha255++; }
        }
        printf("DrawIconEx vs AND mask: ANDset->alpha255=%d alpha0=%d | ANDclear->alpha0=%d alpha255=%d\n",
               andSetAlpha255, andSetAlpha0, andClearAlpha0, andClearAlpha255);
    }

    // 5) what the old force-opaque path produced: how many pixels became opaque that shouldn't be
    if (ok2) {
        int oldForced = 0;   // pixels old code forced opaque (a==0 -> 255)
        for (int i = 0; i < w * h; ++i)
            if (p.iconBits[i * 4 + 3] == 0 && !(p.andMask[i])) oldForced++;
        printf("OLD-code bug size: pixels with alpha==0 AND not AND-masked (became opaque black) = %d (%d%% of %d)\n",
               oldForced, (int)(oldForced * 100 / (w * h)), w * h);
    }

    if (ii.hbmColor) DeleteObject(ii.hbmColor);
    if (ii.hbmMask)  DeleteObject(ii.hbmMask);
    printf("=== done ===\n");
    return 0;
}
