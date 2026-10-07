#pragma once
// ui/modern_ui.h -- 2026-style dark neon UI helpers (pure Win32/GDI, no external deps)
//
// Owner-draw helpers for a modern dark UI:
//   * dark palette (deep slate bg, neon gradient accents matching the new icons)
//   * rounded-rect gradient fills with soft borders (GDI: CreateRoundRectRgn + clip)
//   * button drawing (accent gradient / ghost variants, hover+pressed states)
//   * checkbox drawing (rounded box, gradient check)
//   * combo/listbox item drawing (selection bar, hover)
//   * text-field look via WM_CTLCOLOR (dark bg)
//   * DWM: immersive dark title bar + Win11 rounded corners
//
// Usage: owner-draw controls set BS_OWNERDRAW (buttons/checkboxes) /
// CBS_OWNERDRAWFIXED|CBS_HASSTRINGS (combo) / LBS_OWNERDRAWFIXED|LBS_HASSTRINGS (listbox),
// the wndproc handles WM_DRAWITEM / WM_MEASUREITEM / WM_CTLCOLOR*, and a WM_MOUSEMOVE
// handler calls MuTrackHover() to drive hover states (needs BS_NOTIFY on the controls).

#include <windows.h>
#include <dwmapi.h>
#include <cmath>
#include <cstdint>
#include <cwchar>

#pragma comment(lib, "dwmapi.lib")
#pragma comment(lib, "gdi32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "msimg32.lib")   // AlphaBlend

namespace mu {

// ---------------- palette ----------------
// deep space backgrounds (match icon dark base)
constexpr COLORREF kBgDark      = RGB(0x13, 0x15, 0x23);   // window base
constexpr COLORREF kBgPanel     = RGB(0x1B, 0x1E, 0x30);   // panels / controls
constexpr COLORREF kBgPanel2    = RGB(0x22, 0x26, 0x3B);   // hover / list item
constexpr COLORREF kInput       = RGB(0x0E, 0x10, 0x1C);   // text fields (edit bg)
constexpr COLORREF kBorder      = RGB(0x2E, 0x33, 0x50);   // soft border
constexpr COLORREF kBorderHot   = RGB(0x4A, 0x51, 0x7E);   // hover border
constexpr COLORREF kTextMain    = RGB(0xE6, 0xE9, 0xF5);
constexpr COLORREF kTextDim     = RGB(0x8A, 0x92, 0xB0);
constexpr COLORREF kTextFaint   = RGB(0x5C, 0x64, 0x84);
constexpr COLORREF kCheckMark   = RGB(0x0E, 0x11, 0x1E);

// neon gradients
constexpr COLORREF kNeonA1 = RGB(0x34, 0xD3, 0xEE);   // cyan
constexpr COLORREF kNeonA2 = RGB(0x63, 0x66, 0xF1);   // indigo
constexpr COLORREF kNeonB1 = RGB(0xFB, 0x92, 0x3C);   // orange
constexpr COLORREF kNeonB2 = RGB(0xEC, 0x48, 0x99);   // pink
constexpr COLORREF kSelBar = RGB(0x22, 0xD3, 0xEE);   // selection accent bar

// ---------------- vector icons (pure GDI, no assets) ----------------
enum MuIcon { kIconNone = 0, kIconPlay, kIconStop, kIconCheck, kIconDown, kIconLink };

// draw a small vector icon centered at (cx, cy) in an s x s box
inline void DrawIcon(HDC hdc, int cx, int cy, int size, MuIcon icon, COLORREF color);

// ---------------- low-level drawing ----------------

inline COLORREF lerpColor(COLORREF a, COLORREF b, float t) {
    int r = (int)(GetRValue(a) + (GetRValue(b) - GetRValue(a)) * t);
    int g = (int)(GetGValue(a) + (GetGValue(b) - GetGValue(a)) * t);
    int bl = (int)(GetBValue(a) + (GetBValue(b) - GetBValue(a)) * t);
    return RGB(r, g, bl);
}

// vertical gradient fill over a whole rect (dialog backgrounds, band-by-band)
inline void GradientFillV(HDC hdc, const RECT& rc, COLORREF top, COLORREF bottom, int bands = 24) {
    int h = rc.bottom - rc.top;
    if (h <= 0) return;
    if (bands < 1) bands = 1;
    for (int i = 0; i < bands; ++i) {
        float t = bands > 1 ? (float)i / (bands - 1) : 0.f;
        HBRUSH br = CreateSolidBrush(lerpColor(top, bottom, t));
        int y0 = rc.top + h * i / bands;
        int y1 = rc.top + h * (i + 1) / bands;
        if (y1 <= y0) y1 = y0 + 1;
        RECT r{ rc.left, y0, rc.right, y1 };
        FillRect(hdc, &r, br);
        DeleteObject(br);
    }
}

// fill a rounded rect with a vertical two-color gradient
inline void FillRoundGradient(HDC hdc, const RECT& rc, int radius, COLORREF c1, COLORREF c2) {
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;
    HRGN rgn = CreateRoundRectRgn(rc.left, rc.top, rc.right + 1, rc.bottom + 1, radius * 2, radius * 2);
    int saved = SaveDC(hdc);
    SelectClipRgn(hdc, rgn);
    // 4px gradient bands (fast, smooth enough for small controls)
    const int band = 4;
    for (int y = rc.top; y < rc.bottom; y += band) {
        float t = h > 1 ? (float)(y - rc.top) / (h - 1) : 0.f;
        HBRUSH br = CreateSolidBrush(lerpColor(c1, c2, t));
        int y2 = (y + band < rc.bottom) ? (y + band) : rc.bottom;   // NOMINMAX: no std::min/max macros
        RECT r{ rc.left, y, rc.right, y2 };
        FillRect(hdc, &r, br);
        DeleteObject(br);
    }
    RestoreDC(hdc, saved);
    DeleteObject(rgn);
}

inline void FillRound(HDC hdc, const RECT& rc, int radius, COLORREF c) {
    HRGN rgn = CreateRoundRectRgn(rc.left, rc.top, rc.right + 1, rc.bottom + 1, radius * 2, radius * 2);
    HBRUSH br = CreateSolidBrush(c);
    FillRgn(hdc, rgn, br);
    DeleteObject(br);
    DeleteObject(rgn);
}

inline void StrokeRound(HDC hdc, const RECT& rc, int radius, COLORREF c, int width = 1) {
    for (int i = 0; i < width; ++i) {
        int rx = (radius - i) * 2; if (rx < 1) rx = 1;
        HRGN rgn = CreateRoundRectRgn(rc.left + i, rc.top + i, rc.right - i, rc.bottom - i, rx, rx);
        HBRUSH br = CreateSolidBrush(c);
        FrameRgn(hdc, rgn, br, 1, 1);
        DeleteObject(br);
        DeleteObject(rgn);
    }
}

// draw a small vector icon centered at (cx, cy) in an s x s box (defined after FillRound)
inline void DrawIcon(HDC hdc, int cx, int cy, int size, MuIcon icon, COLORREF color) {
    if (icon == kIconNone || size <= 1) return;
    const int hw = size / 2;
    HBRUSH br = CreateSolidBrush(color);
    HPEN pen = CreatePen(PS_SOLID, 1, color);
    HGDIOBJ ob = SelectObject(hdc, br);
    HGDIOBJ op = SelectObject(hdc, pen);
    switch (icon) {
    case kIconPlay: {   // filled right-pointing triangle
        POINT p[3] = { { cx - hw + 1, cy - hw }, { cx - hw + 1, cy + hw }, { cx + hw - 1, cy } };
        Polygon(hdc, p, 3);
        break;
    }
    case kIconStop: {   // filled rounded square
        int t = size / 4; if (t < 2) t = 2;
        RECT r{ cx - hw + t, cy - hw + t, cx + hw - t, cy + hw - t };
        FillRound(hdc, r, 2, color);
        break;
    }
    case kIconCheck: {  // check mark
        HPEN cpen = CreatePen(PS_SOLID, 2, color);
        HGDIOBJ cop = SelectObject(hdc, cpen);
        MoveToEx(hdc, cx - hw, cy - 1, nullptr);
        LineTo(hdc, cx - 1, cy + hw - 2);
        LineTo(hdc, cx + hw, cy - hw + 1);
        SelectObject(hdc, cop);
        DeleteObject(cpen);
        break;
    }
    case kIconDown: {   // download: down arrow into a tray
        HPEN apen = CreatePen(PS_SOLID, 2, color);
        HGDIOBJ aop = SelectObject(hdc, apen);
        MoveToEx(hdc, cx, cy - hw + 2, nullptr);
        LineTo(hdc, cx, cy + hw - 4);
        LineTo(hdc, cx - 3, cy + hw - 6);
        MoveToEx(hdc, cx, cy + hw - 4, nullptr);
        LineTo(hdc, cx + 3, cy + hw - 6);
        MoveToEx(hdc, cx - hw + 2, cy + hw - 4, nullptr);
        LineTo(hdc, cx + hw - 2, cy + hw - 4);
        SelectObject(hdc, aop);
        DeleteObject(apen);
        break;
    }
    case kIconLink: {   // right arrow (connect)
        HPEN apen = CreatePen(PS_SOLID, 2, color);
        HGDIOBJ aop = SelectObject(hdc, apen);
        MoveToEx(hdc, cx - hw, cy, nullptr);
        LineTo(hdc, cx + hw - 1, cy);
        LineTo(hdc, cx + hw - 4, cy - 3);
        MoveToEx(hdc, cx + hw - 1, cy, nullptr);
        LineTo(hdc, cx + hw - 4, cy + 3);
        SelectObject(hdc, aop);
        DeleteObject(apen);
        break;
    }
    default: break;
    }
    SelectObject(hdc, ob);
    SelectObject(hdc, op);
    DeleteObject(br);
    DeleteObject(pen);
}

// translucent rounded overlay (hover/pressed tint) via AlphaBlend -- opaque FillRound would
// hide the gradient underneath and make the label unreadable (white-on-white / black-on-black)
inline void OverlayRound(HDC hdc, const RECT& rc, int radius, COLORREF c, BYTE alpha) {
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (w <= 0 || h <= 0) return;
    HDC mem = CreateCompatibleDC(hdc);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;   // top-down
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, dib);
    HBRUSH br = CreateSolidBrush(c);
    HRGN rgn = CreateRoundRectRgn(0, 0, w + 1, h + 1, radius * 2, radius * 2);
    FillRgn(mem, rgn, br);
    DeleteObject(br);
    DeleteObject(rgn);
    BLENDFUNCTION bf{ AC_SRC_OVER, 0, alpha, 0 };   // constant-alpha blend, no per-pixel alpha
    AlphaBlend(hdc, rc.left, rc.top, w, h, mem, 0, 0, w, h, bf);
    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
}

inline void DrawLabel(HDC hdc, const RECT& rc, const wchar_t* text, COLORREF color, HFONT font,
                      UINT fmt = DT_CENTER | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS) {
    SetTextColor(hdc, color);
    SetBkMode(hdc, TRANSPARENT);
    HGDIOBJ old = font ? SelectObject(hdc, font) : nullptr;
    DrawTextW(hdc, text, -1, (LPRECT)&rc, fmt);
    if (old) SelectObject(hdc, old);
}

// ---------------- atomic (double-buffered) drawing ----------------
// All owner-draw shapes below render into an off-screen DIB first, then commit with ONE
// BitBlt. Reason: the teacher app captures the desktop (preview pane + student video) at
// ~30fps. A multi-step on-screen repaint (gradient bands + text) leaves the control in a
// partially-painted state for ~1-2ms, which the capture can sample and encode into the
// video as text ghosting/overlap ("字体重叠" when toggling the start/stop button).
// Double-buffering shrinks the on-screen change to a single ~5us blit, so the capture
// essentially never sees a torn frame.
struct MuAtomic {
    HDC mem = nullptr;
    HBITMAP dib = nullptr;
    HGDIOBJ old = nullptr;
    int w = 0, h = 0;
};

inline MuAtomic MuAtomicBegin(HDC hdc, const RECT& rc, COLORREF clear) {
    MuAtomic a{};
    a.w = rc.right - rc.left; a.h = rc.bottom - rc.top;
    if (a.w <= 0 || a.h <= 0) return a;
    a.mem = CreateCompatibleDC(hdc);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = a.w;
    bi.bmiHeader.biHeight = -a.h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    a.dib = CreateDIBSection(a.mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    if (!a.dib) { DeleteDC(a.mem); a.mem = nullptr; return a; }
    a.old = (HGDIOBJ)SelectObject(a.mem, a.dib);
    RECT full{ 0, 0, a.w, a.h };
    HBRUSH br = CreateSolidBrush(clear);
    FillRect(a.mem, &full, br);
    DeleteObject(br);
    // NOTE: no viewport/window origin tricks. The draw functions below draw with 0-based
    // RELATIVE coords so that GDI region objects (device-space) and FillRect/DrawTextW
    // (logical-space) both land on the (0..w, 0..h) surface consistently.
    return a;
}

inline void MuAtomicEnd(HDC hdc, const RECT& rc, MuAtomic& a) {
    if (!a.mem) return;
    BitBlt(hdc, rc.left, rc.top, a.w, a.h, a.mem, 0, 0, SRCCOPY);
    SelectObject(a.mem, a.old);
    DeleteObject(a.dib);
    DeleteDC(a.mem);
    a.mem = nullptr; a.dib = nullptr;
}

// ---------------- owner-draw item shapes ----------------

// standard button: accent gradient fill (primary) or dark ghost with border (secondary).
// Optional vector icon drawn left of the (group-centered) label.
inline void DrawButton(HDC hdc, const RECT& rc, const wchar_t* text, bool hover, bool pressed,
                       bool accent, bool gradientIsB /* orange-pink vs cyan-indigo */, HFONT font,
                       MuIcon icon = kIconNone) {
    MuAtomic a = MuAtomicBegin(hdc, rc, kBgPanel);
    if (!a.mem) return;
    HDC d = a.mem;
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    const int radius = h / 2;   // pill shape
    RECT body{ 0, 0, w, h };    // 0-based relative: consistent with GDI regions (device coords)
    if (pressed) { body.top += 1; body.left += 1; body.right -= 1; body.bottom -= 1; }

    // draw the label, optionally with a vector icon left of it (icon + gap + text centered as one group)
    auto drawLabelIcon = [&](COLORREF lc) {
        if (icon == kIconNone) { DrawLabel(d, body, text, lc, font); return; }
        int isz = h >= 26 ? 16 : 12;
        int gap = 7;
        SIZE tx{};
        HGDIOBJ oldF = font ? SelectObject(d, font) : nullptr;
        GetTextExtentPoint32W(d, text, (int)wcslen(text), &tx);
        if (oldF) SelectObject(d, oldF);
        int group = isz + gap + tx.cx;
        int x0 = body.left + (w - group) / 2;
        if (x0 < body.left + 2) x0 = body.left + 2;
        RECT tr{ x0 + isz + gap, body.top, body.right, body.bottom };
        DrawLabel(d, tr, text, lc, font, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
        DrawIcon(d, x0 + isz / 2, body.top + (body.bottom - body.top) / 2, isz, icon, lc);
    };

    if (accent) {
        COLORREF c1 = gradientIsB ? kNeonB1 : kNeonA1;
        COLORREF c2 = gradientIsB ? kNeonB2 : kNeonA2;
        FillRoundGradient(d, body, radius, c1, c2);
        if (pressed) {
            OverlayRound(d, body, radius, RGB(0, 0, 0), 60);            // dim toward black (translucent)
            StrokeRound(d, body, radius, kBorderHot, 1);
        } else if (hover) {
            OverlayRound(d, body, radius, RGB(255, 255, 255), 40);      // brighten (translucent)
        }
        drawLabelIcon(pressed ? RGB(0xC8, 0xD0, 0xE8) : RGB(0xFF, 0xFF, 0xFF));
    } else {
        FillRound(d, body, radius, hover ? kBgPanel2 : kBgPanel);
        StrokeRound(d, body, radius, hover ? kBorderHot : kBorder, 1);
        drawLabelIcon(pressed ? kTextDim : kTextMain);
    }
    MuAtomicEnd(hdc, rc, a);
}

// custom checkbox: rounded square + gradient check when on
inline void DrawCheckbox(HDC hdc, const RECT& rc, const wchar_t* text, bool checked, bool hover,
                         bool gradientIsB, HFONT font) {
    MuAtomic a = MuAtomicBegin(hdc, rc, kBgPanel);
    if (!a.mem) return;
    HDC d = a.mem;
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    int box = h - 8; if (box < 14) box = 14;
    if (box > h) box = h;   // guard tiny controls
    RECT br{ 0, (h - box) / 2, box, (h - box) / 2 + box };   // 0-based relative
    const int r = 4;

    if (checked) {
        // 选中: 亮色渐变填充 + 白色粗对勾 + 亮边框。必须一眼可辨: 之前的深色对勾
        // (kCheckMark) 在渐变下缘(靛蓝)上几乎看不见, 选框看起来只是"亮色块"。
        COLORREF c1 = gradientIsB ? kNeonB1 : kNeonA1;
        COLORREF c2 = gradientIsB ? kNeonB2 : kNeonA2;
        FillRoundGradient(d, br, r, c1, c2);
        StrokeRound(d, br, r, RGB(0xE8, 0xEE, 0xFF), 1);   // bright border pops the box
        // white check mark (max contrast on the neon gradient)
        int penW = box / 5; if (penW < 2) penW = 2;
        HPEN pen = CreatePen(PS_SOLID, penW, RGB(0xFF, 0xFF, 0xFF));
        HGDIOBJ oldPen = SelectObject(d, pen);
        MoveToEx(d, br.left + box / 4, br.top + box / 2, nullptr);
        LineTo(d, br.left + box / 2, br.top + box * 3 / 4);
        LineTo(d, br.right - box / 5, br.top + box / 4);
        SelectObject(d, oldPen);
        DeleteObject(pen);
    } else {
        FillRound(d, br, r, kBgPanel2);
        StrokeRound(d, br, r, hover ? kBorderHot : kBorder, 1);
    }
    if (hover && !checked) StrokeRound(d, br, r, kBorderHot, 1);

    RECT tr{ br.right + 8, 0, w, h };   // 0-based relative
    DrawLabel(d, tr, text, checked ? kTextMain : kTextDim, font, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    MuAtomicEnd(hdc, rc, a);
}

// combo/list item: selection bar + hover bg
// custom radio (circle + dot) -- used by the installer's single-select group
inline void DrawRadio(HDC hdc, const RECT& rc, const wchar_t* text, bool checked, bool hover,
                      bool gradientIsB, HFONT font) {
    MuAtomic a = MuAtomicBegin(hdc, rc, kBgPanel);
    if (!a.mem) return;
    HDC d = a.mem;
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    int dsz = h - 8; if (dsz < 14) dsz = 14;
    RECT cr{ 0, (h - dsz) / 2, dsz, (h - dsz) / 2 + dsz };   // 0-based relative
    if (checked) {
        COLORREF c1 = gradientIsB ? kNeonB1 : kNeonA1;
        COLORREF c2 = gradientIsB ? kNeonB2 : kNeonA2;
        HBRUSH br = CreateSolidBrush(c1);
        HGDIOBJ oldBr = SelectObject(d, br);
        HPEN pen = CreatePen(PS_SOLID, 1, c2);
        HGDIOBJ oldPen = SelectObject(d, pen);
        Ellipse(d, cr.left, cr.top, cr.right, cr.bottom);
        SelectObject(d, oldBr); DeleteObject(br);
        SelectObject(d, oldPen); DeleteObject(pen);
        // inner dot
        int pad = dsz / 4;
        HBRUSH dot = CreateSolidBrush(c2);
        HGDIOBJ oldDot = SelectObject(d, dot);
        HGDIOBJ oldNP = SelectObject(d, GetStockObject(NULL_PEN));
        Ellipse(d, cr.left + pad, cr.top + pad, cr.right - pad, cr.bottom - pad);
        SelectObject(d, oldDot); DeleteObject(dot);
        SelectObject(d, oldNP);
    } else {
        HBRUSH br = CreateSolidBrush(kBgPanel2);
        HGDIOBJ oldBr = SelectObject(d, br);
        HPEN pen = CreatePen(PS_SOLID, 1, hover ? kBorderHot : kBorder);
        HGDIOBJ oldPen = SelectObject(d, pen);
        Ellipse(d, cr.left, cr.top, cr.right, cr.bottom);
        SelectObject(d, oldBr); DeleteObject(br);
        SelectObject(d, oldPen); DeleteObject(pen);
    }
    RECT tr{ cr.right + 8, 0, w, h };   // 0-based relative
    DrawLabel(d, tr, text, checked ? kTextMain : kTextDim, font, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    MuAtomicEnd(hdc, rc, a);
}

inline void DrawListItem(HDC hdc, const RECT& rc, const wchar_t* text, bool selected, bool hover,
                         bool accentBarIsB, HFONT font) {
    MuAtomic a = MuAtomicBegin(hdc, rc, kBgPanel);
    if (!a.mem) return;
    HDC d = a.mem;
    const int w = rc.right - rc.left, h = rc.bottom - rc.top;
    RECT body{ 0, 0, w, h };   // 0-based relative
    if (selected) {
        FillRound(d, body, 6, kBgPanel2);
        // left accent bar
        RECT bar{ 2, 3, 5, h - 3 };
        COLORREF c1 = accentBarIsB ? kNeonB1 : kNeonA1;
        COLORREF c2 = accentBarIsB ? kNeonB2 : kNeonA2;
        FillRoundGradient(d, bar, 2, c1, c2);
        RECT tr{ 14, 0, w, h };
        DrawLabel(d, tr, text, kTextMain, font, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    } else {
        if (hover) FillRound(d, body, 6, kBgPanel);
        RECT tr{ 10, 0, w, h };
        DrawLabel(d, tr, text, hover ? kTextMain : kTextDim, font, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
    }
    MuAtomicEnd(hdc, rc, a);
}

// ---------------- dark rounded edit fields ----------------
// Subclasses an EDIT/COMBOBOX to drop the system light border (WS_BORDER) and draw a
// dark rounded border instead: kBorder normally, kBorderHot while focused. The old wndproc
// is stored in GWLP_USERDATA, which is safe because MuTrackHover() below only touches
// owner-draw buttons (never edits).
static LRESULT CALLBACK MuEditProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    WNDPROC old = (WNDPROC)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (msg) {
    case WM_PAINT: {
        LRESULT r = CallWindowProcW(old, h, msg, wp, lp);   // default: bg + text
        HDC dc = GetWindowDC(h);
        RECT rc; GetClientRect(h, &rc);
        StrokeRound(dc, rc, 4, GetFocus() == h ? kBorderHot : kBorder, 1);
        ReleaseDC(h, dc);
        return r;
    }
    case WM_SETFOCUS:
    case WM_KILLFOCUS: {
        LRESULT r = CallWindowProcW(old, h, msg, wp, lp);
        InvalidateRect(h, nullptr, TRUE);   // repaint the border with the focus state
        return r;
    }
    default:
        return CallWindowProcW(old, h, msg, wp, lp);
    }
}

inline void MuStyleEdit(HWND edit) {
    if (!edit) return;
    // drop the system light border; our subclass draws the dark rounded one
    LONG_PTR style = GetWindowLongPtrW(edit, GWL_STYLE);
    if (style & WS_BORDER) {
        SetWindowLongPtrW(edit, GWL_STYLE, style & ~WS_BORDER);
        SetWindowPos(edit, nullptr, 0, 0, 0, 0,
                     SWP_NOMOVE | SWP_NOSIZE | SWP_NOZORDER | SWP_NOACTIVATE | SWP_FRAMECHANGED);
    }
    SetWindowLongPtrW(edit, GWLP_USERDATA, (LONG_PTR)GetWindowLongPtrW(edit, GWLP_WNDPROC));
    SetWindowLongPtrW(edit, GWLP_WNDPROC, (LONG_PTR)MuEditProc);
}

// ---------------- DWM helpers ----------------

// immersive dark title bar + Win11 rounded corners; safe no-ops on older Windows
inline void ApplyDarkFrame(HWND hwnd, bool rounded) {
    BOOL dark = TRUE;
    DwmSetWindowAttribute(hwnd, DWMWA_USE_IMMERSIVE_DARK_MODE, &dark, sizeof(dark));
    if (rounded) {
        DWORD pref = DWMWCP_ROUND;
        DwmSetWindowAttribute(hwnd, DWMWA_WINDOW_CORNER_PREFERENCE, &pref, sizeof(pref));
    }
}

// ---------------- hover tracking ----------------
// Call from the parent's WM_MOUSEMOVE. Owner-draw buttons must have BS_NOTIFY style so the
// parent receives WM_MOUSEMOVE while over them. Uses GWLP_USERDATA bit 0 as hover flag.
inline void MuTrackHover(HWND parent, LPARAM lp) {
    POINT pt{ (short)LOWORD(lp), (short)HIWORD(lp) };
    ClientToScreen(parent, &pt);
    HWND under = WindowFromPoint(pt);
    // request leave notification so hovers are cleared when the cursor exits the window
    TRACKMOUSEEVENT tme{ sizeof(tme), TME_LEAVE, parent, 0 };
    TrackMouseEvent(&tme);
    for (HWND c = GetWindow(parent, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
        // hover tracking is for owner-draw buttons ONLY: checkboxes/radios/start button.
        // Combos/listboxes drive their own hover via ODS_HOTLIGHT; edits store their old
        // wndproc in GWLP_USERDATA, so touching the userdata bits here would corrupt them.
        LONG_PTR st = GetWindowLongPtrW(c, GWL_STYLE);
        if ((st & BS_OWNERDRAW) != BS_OWNERDRAW) continue;
        LONG_PTR flags = GetWindowLongPtrW(c, GWLP_USERDATA);
        bool hover = (flags & 1) != 0;
        bool shouldHover = (under == c);
        if (hover != shouldHover) {
            SetWindowLongPtrW(c, GWLP_USERDATA, shouldHover ? (flags | 1) : (flags & ~1));
            InvalidateRect(c, nullptr, FALSE);
        }
    }
}

// call from the parent's WM_MOUSELEAVE: clear every child hover flag (mouse left the window)
inline void MuClearHover(HWND parent) {
    for (HWND c = GetWindow(parent, GW_CHILD); c; c = GetWindow(c, GW_HWNDNEXT)) {
        LONG_PTR st = GetWindowLongPtrW(c, GWL_STYLE);
        if ((st & BS_OWNERDRAW) != BS_OWNERDRAW) continue;
        LONG_PTR flags = GetWindowLongPtrW(c, GWLP_USERDATA);
        if (flags & 1) {
            SetWindowLongPtrW(c, GWLP_USERDATA, flags & ~1);
            InvalidateRect(c, nullptr, FALSE);
        }
    }
}

inline bool MuIsHover(HWND hwnd) { return (GetWindowLongPtrW(hwnd, GWLP_USERDATA) & 1) != 0; }

} // namespace mu
