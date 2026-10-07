// test_btn_draw.cpp - standalone reproduction of the start/stop button text-toggle.
// Creates the same BS_OWNERDRAW button, toggles text via SetWindowTextW, then renders
// the button into a memory DC via WM_PRINTCLIENT and saves BMPs for pixel analysis.
// Completely screen-independent.
#include <windows.h>
#include <cstdio>
#include <vector>
#include "ui/modern_ui.h"

static HFONT g_font = nullptr;
static HWND g_btn = nullptr;
static HWND g_edit = nullptr;
static HBRUSH g_inputBrush = nullptr;
static HWND g_stMoved = nullptr, g_stLabel = nullptr;   // static probing: moved header vs plain label

static bool saveBitmap(HDC mem, int w, int h, const char* path) {
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HDC tmp = CreateCompatibleDC(mem);
    HBITMAP dib = CreateDIBSection(tmp, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(tmp, dib);
    BitBlt(tmp, 0, 0, w, h, mem, 0, 0, SRCCOPY);
    // BGRA -> write BMP (BGR + alpha ignored)
    BITMAPFILEHEADER fh{};
    fh.bfType = 0x4D42;
    fh.bfOffBits = sizeof(BITMAPFILEHEADER) + sizeof(BITMAPINFOHEADER);
    fh.bfSize = fh.bfOffBits + w * h * 4;
    BITMAPINFOHEADER ih = bi.bmiHeader;
    FILE* f = fopen(path, "wb");
    if (!f) { SelectObject(tmp, old); DeleteObject(dib); DeleteDC(tmp); return false; }
    fwrite(&fh, sizeof(fh), 1, f);
    fwrite(&ih, sizeof(ih), 1, f);
    fwrite(bits, 4, (size_t)w * h, f);
    fclose(f);
    SelectObject(tmp, old);
    DeleteObject(dib);
    DeleteDC(tmp);
    return true;
}

// render a checkbox state into a 88x24 bitmap (same size as the teacher's top-bar checkboxes)
static void renderCheckbox(const char* path, const wchar_t* text, bool checked, bool hover) {
    int w = 88, h = 24;
    HDC mem = CreateCompatibleDC(nullptr);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, dib);
    memset(bits, 0, (size_t)w * h * 4);
    RECT rc{ 0, 0, w, h };
    mu::DrawCheckbox(mem, rc, text, checked, hover, false, g_font);
    saveBitmap(mem, w, h, path);
    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    printf("checkbox %s (checked=%d hover=%d)\n", path, checked ? 1 : 0, hover ? 1 : 0);
}

// direct-draw with explicit hover/pressed (tests the drawing functions themselves).
// When atRect is given, draws into a large surface at that offset (like a real control
// positioned inside its window), then crops the button rect out and saves it.
static void renderDirect(const char* path, bool hover, bool pressed, const RECT* atRect = nullptr) {
    int w = 104, h = 30;
    RECT rc = atRect ? *atRect : RECT{ 0, 0, w, h };
    int surfW = (rc.right > 0 ? rc.right : w);
    int surfH = (rc.bottom > 0 ? rc.bottom : h);
    HDC mem = CreateCompatibleDC(nullptr);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = surfW;
    bi.bmiHeader.biHeight = -surfH;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, dib);
    memset(bits, 0, (size_t)surfW * surfH * 4);
    mu::DrawButton(mem, rc, L"停止推流", hover, pressed, true, false, g_font);
    // crop the button rect from the surface and save just that
    RECT crop = { 0, 0, w, h };
    HDC c = CreateCompatibleDC(mem);
    BITMAPINFO ci{};
    ci.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    ci.bmiHeader.biWidth = w;
    ci.bmiHeader.biHeight = -h;
    ci.bmiHeader.biPlanes = 1;
    ci.bmiHeader.biBitCount = 32;
    ci.bmiHeader.biCompression = BI_RGB;
    void* cbits = nullptr;
    HBITMAP cdib = CreateDIBSection(c, &ci, DIB_RGB_COLORS, &cbits, nullptr, 0);
    HGDIOBJ cold = SelectObject(c, cdib);
    BitBlt(c, 0, 0, w, h, mem, rc.left, rc.top, SRCCOPY);
    saveBitmap(c, w, h, path);
    SelectObject(c, cold);
    DeleteObject(cdib);
    DeleteDC(c);
    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    printf("direct-drew %s rect=(%d,%d,%d,%d)\n", path, rc.left, rc.top, rc.right, rc.bottom);
}

// direct-draw a button with an optional vector icon and accent/ghost style
static void renderDirectIcon(const char* path, const wchar_t* text, bool hover, bool pressed,
                             bool accent, mu::MuIcon icon) {
    int w = 104, h = 30;
    HDC mem = CreateCompatibleDC(nullptr);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, dib);
    memset(bits, 0, (size_t)w * h * 4);
    RECT rc{ 0, 0, w, h };
    mu::DrawButton(mem, rc, text, hover, pressed, accent, false, g_font, icon);
    saveBitmap(mem, w, h, path);
    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    printf("direct-drew icon %s\n", path);
}

// render any control (button/edit) into a memory DC (screen-independent) via WM_PRINTCLIENT
static void renderControl(const char* path, HWND ctl) {
    RECT rc; GetClientRect(ctl, &rc);
    int w = rc.right, h = rc.bottom;
    HDC mem = CreateCompatibleDC(nullptr);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, dib);
    memset(bits, 0, (size_t)w * h * 4);
    SendMessageW(ctl, WM_PRINTCLIENT, (WPARAM)mem, PRF_CLIENT | PRF_ERASEBKGND | PRF_CHILDREN);
    saveBitmap(mem, w, h, path);
    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    printf("rendered %s (%dx%d)\n", path, w, h);
}

// render the button control into a memory DC (screen-independent) via WM_PRINTCLIENT
static void renderButton(const char* path, bool hover, bool pressed) {
    RECT rc; GetClientRect(g_btn, &rc);
    int w = rc.right, h = rc.bottom;
    HDC mem = CreateCompatibleDC(nullptr);
    BITMAPINFO bi{};
    bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
    bi.bmiHeader.biWidth = w;
    bi.bmiHeader.biHeight = -h;
    bi.bmiHeader.biPlanes = 1;
    bi.bmiHeader.biBitCount = 32;
    bi.bmiHeader.biCompression = BI_RGB;
    void* bits = nullptr;
    HBITMAP dib = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
    HGDIOBJ old = SelectObject(mem, dib);
    // clear to black
    memset(bits, 0, (size_t)w * h * 4);
    // force the control to render into our DC
    SendMessageW(g_btn, WM_PRINTCLIENT, (WPARAM)mem, PRF_CLIENT | PRF_ERASEBKGND | PRF_CHILDREN);
    saveBitmap(mem, w, h, path);
    SelectObject(mem, old);
    DeleteObject(dib);
    DeleteDC(mem);
    printf("rendered %s (%dx%d)\n", path, w, h);
}

static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                             OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");
        g_btn = CreateWindowW(L"BUTTON", L"开始推流", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | BS_NOTIFY,
                              10, 10, 104, 30, hwnd, (HMENU)104, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(g_btn, WM_SETFONT, (WPARAM)g_font, TRUE);
        // dark rounded edit (MuStyleEdit) -- same style as the app's IP field
        g_inputBrush = CreateSolidBrush(mu::kInput);
        g_edit = CreateWindowW(L"EDIT", L"239.255.1.1", WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                               10, 60, 150, 28, hwnd, (HMENU)106, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(g_edit, WM_SETFONT, (WPARAM)g_font, TRUE);
        mu::MuStyleEdit(g_edit);
        // static probe: header-like (moved via SetWindowPos) vs label-like (created in place)
        g_stMoved = CreateWindowW(L"STATIC", L"客户端 (0)", WS_CHILD | WS_VISIBLE, 0, 0, 186, 20, hwnd, (HMENU)208, nullptr, nullptr);
        SetWindowPos(g_stMoved, nullptr, 594, 80, 166, 20, SWP_NOZORDER | SWP_NOACTIVATE);
        g_stLabel = CreateWindowW(L"STATIC", L"教师IP", WS_CHILD | WS_VISIBLE, 10, 100, 52, 12, hwnd, (HMENU)209, nullptr, nullptr);
        return 0;
    }
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT* di = (DRAWITEMSTRUCT*)lp;
        if (di->CtlType == ODT_BUTTON) {
            wchar_t txt[64]; GetWindowTextW(di->hwndItem, txt, 64);
            bool hover = (di->itemState & ODS_HOTLIGHT) != 0;
            bool pressed = (di->itemState & ODS_SELECTED) != 0;
            mu::DrawButton(di->hDC, di->rcItem, txt, hover, pressed, true, false, g_font);
            return TRUE;
        }
        return FALSE;
    }
    case WM_CTLCOLOREDIT: {
        // like the real app: dark input background
        HDC hdc = (HDC)wp;
        SetTextColor(hdc, mu::kTextMain);
        SetBkColor(hdc, mu::kInput);
        return (LRESULT)g_inputBrush;
    }
    case WM_CTLCOLORSTATIC: {
        // like the real app: transparent statics (NULL_BRUSH)
        HDC hdc = (HDC)wp;
        SetTextColor(hdc, mu::kTextDim);
        SetBkColor(hdc, mu::kBgDark);
        SetBkMode(hdc, TRANSPARENT);
        return (LRESULT)GetStockObject(NULL_BRUSH);
    }
    case WM_DESTROY:
        if (g_font) DeleteObject(g_font);
        if (g_inputBrush) DeleteObject(g_inputBrush);
        PostQuitMessage(0);
        return 0;
    }
    return DefWindowProcW(hwnd, msg, wp, lp);
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    SetProcessDPIAware();
    WNDCLASSW wc = {};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.lpszClassName = L"BtnDrawTest";
    RegisterClassW(&wc);
    HWND hwnd = CreateWindowExW(0, L"BtnDrawTest", L"btn draw test",
                                WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU, CW_USEDEFAULT, CW_USEDEFAULT,
                                200, 120, nullptr, nullptr, hInst, nullptr);
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    // ensure the button paints once with its initial text (开始推流)
    RedrawWindow(g_btn, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    renderButton("msi/t_initial.bmp", false, false);

    // toggle to 停止推流 (like startStreaming does)
    SetWindowTextW(g_btn, L"停止推流");
    RedrawWindow(g_btn, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    renderButton("msi/t_stop.bmp", false, false);

    // toggle back to 开始推流
    SetWindowTextW(g_btn, L"开始推流");
    RedrawWindow(g_btn, nullptr, nullptr, RDW_INVALIDATE | RDW_UPDATENOW | RDW_ALLCHILDREN);
    renderButton("msi/t_start.bmp", false, false);

    // hover + pressed states on 停止推流 (direct draw of the drawing functions)
    renderDirect("msi/t_hover.bmp", true, false);
    renderDirect("msi/t_pressed.bmp", false, true);
    renderDirect("msi/t_normal.bmp", false, false);

    // non-zero origin: must render pixel-identically to the 0-based render (regions are
    // device-space, so this validates the relative-coord approach for the real app's rects)
    RECT realRect{ 660, 8, 764, 38 };
    renderDirect("msi/t_offset.bmp", false, false, &realRect);

    // checkbox states (88x24 like the teacher top bar) for visual verification
    renderCheckbox("msi/cb_checked.bmp", L"差分投屏", true, false);
    renderCheckbox("msi/cb_unchecked.bmp", L"变化区域", false, false);
    renderCheckbox("msi/cb_checked_hover.bmp", L"组播模式", true, true);
    renderCheckbox("msi/cb_unchecked_hover.bmp", L"智能码率", false, true);

    // vector icons on the start/stop/connect/install buttons (accent + ghost styles)
    renderDirectIcon("msi/t_play.bmp", L"开始推流", false, false, true, mu::kIconPlay);
    renderDirectIcon("msi/t_stopicon.bmp", L"停止推流", false, false, true, mu::kIconStop);
    renderDirectIcon("msi/t_link.bmp", L"连接", false, false, true, mu::kIconLink);
    renderDirectIcon("msi/t_down.bmp", L"安装", false, false, true, mu::kIconDown);
    renderDirectIcon("msi/t_ghost.bmp", L"取消", false, false, false, mu::kIconNone);
    renderDirectIcon("msi/t_ghost_check.bmp", L"完成", false, false, false, mu::kIconCheck);

    // static probe: moved-header vs plain-label transparency under NULL_BRUSH
    renderControl("msi/t_st_moved.bmp", g_stMoved);
    renderControl("msi/t_st_label.bmp", g_stLabel);

    // MuStyleEdit assertions: system border removed + wndproc subclassed
    {
        LONG_PTR st = GetWindowLongPtrW(g_edit, GWL_STYLE);
        LONG_PTR wp_ = GetWindowLongPtrW(g_edit, GWLP_WNDPROC);
        printf("edit style WS_BORDER removed: %s\n", (st & WS_BORDER) ? "NO (FAIL)" : "yes");
        printf("edit subclassed: %s\n", wp_ == (LONG_PTR)mu::MuEditProc ? "yes" : "NO (FAIL)");
    }
    // dark rounded edit render: dark bg (CTLCOLOREDIT) + dark rounded border (same call MuEditProc::WM_PAINT makes)
    renderControl("msi/t_edit.bmp", g_edit);
    {
        RECT rc; GetClientRect(g_edit, &rc);
        int w = rc.right, h = rc.bottom;
        HDC mem = CreateCompatibleDC(nullptr);
        BITMAPINFO bi{};
        bi.bmiHeader.biSize = sizeof(BITMAPINFOHEADER);
        bi.bmiHeader.biWidth = w;
        bi.bmiHeader.biHeight = -h;
        bi.bmiHeader.biPlanes = 1;
        bi.bmiHeader.biBitCount = 32;
        bi.bmiHeader.biCompression = BI_RGB;
        void* bits = nullptr;
        HBITMAP dib = CreateDIBSection(mem, &bi, DIB_RGB_COLORS, &bits, nullptr, 0);
        HGDIOBJ old = SelectObject(mem, dib);
        SendMessageW(g_edit, WM_PRINTCLIENT, (WPARAM)mem, PRF_CLIENT | PRF_ERASEBKGND);
        mu::StrokeRound(mem, rc, 4, mu::kBorder, 1);   // MuEditProc::WM_PAINT border
        saveBitmap(mem, w, h, "msi/t_edit.bmp");
        SelectObject(mem, old);
        DeleteObject(dib);
        DeleteDC(mem);
    }

    DestroyWindow(hwnd);
    printf("DONE\n");
    return 0;
}
