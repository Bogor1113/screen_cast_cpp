// teacher_gui.cpp —— teacher GUI (preview + quality + differential + multicast mode)
// usage: teacher_gui.exe [clientIP] [bitrateKbps]
// note: winsock2.h must precede windows.h; all wide-char APIs to display Chinese
#include <winsock2.h>
#include <ws2tcpip.h>
#include <windows.h>
#define _WIN32_IE 0x0500
#include <commctrl.h>
#include <shellapi.h>
#include <cstdio>
#include <cstring>
#include <string>
#include <thread>
#include <vector>
#include <dbghelp.h>
#include <iphlpapi.h>

#pragma comment(lib, "iphlpapi.lib")

// crash dump: on an unhandled exception write the address to a file for locating
static LONG WINAPI crashHandler(EXCEPTION_POINTERS* ep) {
    FILE* f = fopen("crash_dump.txt", "w");
    if (f) {
        fprintf(f, "code=0x%X addr=%p\n", ep->ExceptionRecord->ExceptionCode,
                ep->ExceptionRecord->ExceptionAddress);
        fclose(f);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}
#include "core/streamer.h"
#include "ui/preview.h"
#include "ui/modern_ui.h"
#include "version.h"

#pragma comment(lib, "comctl32.lib")
#pragma comment(linker,"/manifestdependency:\"type='win32' name='Microsoft.Windows.Common-Controls' version='6.0.0.0' processorArchitecture='*' publicKeyToken='6595b64144ccf1df' language='*'\"")

// dark neon palette (2026): deep slate bg + neon gradient accents, matches the new icons
static const COLORREF kBg     = mu::kBgDark;
static const COLORREF kPanel  = mu::kBgPanel;
static const COLORREF kText   = mu::kTextMain;
static const COLORREF kDim    = mu::kTextDim;

#define IDC_QUALITY    101
#define IDC_DIFF       102
#define IDC_DIRTYS     103
#define IDC_BTN_START  104
#define IDC_STATS      105
#define IDC_IP_EDIT    106
#define IDC_MCAST      107
#define IDC_CLIENTS    108
#define IDC_CLIENT_LB  109
#define IDC_ADAPTIVE   110
#define IDC_LBL_IP     111
#define IDC_LBL_Q      112

// auto-detect the local non-loopback IPv4 address (read NICs directly via GetAdaptersInfo)
static void autoDetectIP(wchar_t* out, size_t outLen) {
    ULONG bufLen = sizeof(IP_ADAPTER_INFO) * 16;
    std::vector<uint8_t> buf(bufLen);
    IP_ADAPTER_INFO* adapters = (IP_ADAPTER_INFO*)buf.data();
    if (GetAdaptersInfo(adapters, &bufLen) == ERROR_BUFFER_OVERFLOW) {
        buf.resize(bufLen);
        adapters = (IP_ADAPTER_INFO*)buf.data();
    }
    if (GetAdaptersInfo(adapters, &bufLen) == NO_ERROR) {
        for (auto* a = adapters; a; a = a->Next) {
            if (a->IpAddressList.IpAddress.String[0] &&
                strncmp(a->IpAddressList.IpAddress.String, "127.", 4) != 0 &&
                strncmp(a->IpAddressList.IpAddress.String, "0.0.", 4) != 0) {
                MultiByteToWideChar(CP_UTF8, 0, a->IpAddressList.IpAddress.String, -1, out, (int)outLen);
                return;
            }
        }
    }
    wcscpy_s(out, outLen, L"127.0.0.1");
}
static const wchar_t* kDefaultGroup   = L"239.255.1.1";

struct App {
    Streamer streamer;
    PreviewBuffer preview;
    HWND hWnd = nullptr, hStats = nullptr, hQuality = nullptr, hDiff = nullptr, hDirty = nullptr, hStart = nullptr, hIpEdit = nullptr, hMc = nullptr;
    HWND hLblIp = nullptr, hLblQ = nullptr;   // top-bar labels
    HWND hAdaptive = nullptr;                                  // adaptive bitrate checkbox
    HWND hClients = nullptr, hClientLb = nullptr;              // client-list panel (count + IPs)
    std::vector<std::string> lastClients;                      // cached client list (rebuild controls only on change)
    std::wstring targetIP = L"127.0.0.1";
    bool started = false;
    HFONT font = nullptr;
    HBRUSH bgBrush = nullptr, panelBrush = nullptr;
    UINT_PTR timer = 1;
    // double buffer (avoid flashing during intermediate paint states)
    HDC memDc = nullptr;
    HBITMAP memBmp = nullptr;
    HGDIOBJ memOld = nullptr;
    int memW = 0, memH = 0;
    // refresh throttling (preview ~15fps, status bar ~2Hz)
    uint64_t lastPreviewMs = 0, lastPreviewSeq = 0, lastStatsMs = 0;
};

static App g_app;

static RECT previewRect();   // defined below (client coords)
// the preview pane's on-screen rect: the ONLY part of the teacher window that mirrors the desktop live.
// Excluding exactly this region (instead of the whole window) keeps the student picture recursion-free
// while still showing the teacher window's interface (title bar, buttons, client list) to students.
static RECT previewScreenRect() {
    RECT pr = previewRect();
    POINT tl{ pr.left, pr.top }, br{ pr.right, pr.bottom };
    ClientToScreen(g_app.hWnd, &tl);
    ClientToScreen(g_app.hWnd, &br);
    return RECT{ tl.x, tl.y, br.x, br.y };
}

static int comboCurSel(HWND h) { return (int)SendMessageW(h, CB_GETCURSEL, 0, 0); }
static bool btnChecked(HWND h) { return SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED; }
static void comboAdd(HWND h, const wchar_t* s) { SendMessageW(h, CB_ADDSTRING, 0, (LPARAM)s); }
static void comboSetSel(HWND h, int i) { SendMessageW(h, CB_SETCURSEL, (WPARAM)i, 0); }
static void btnSetCheck(HWND h, bool on) { SendMessageW(h, BM_SETCHECK, on ? BST_CHECKED : BST_UNCHECKED, 0); }

static Quality qualityFromIndex(int idx) {
    return idx <= 0 ? Quality::ORIGINAL : Quality::HD1080P;
}

static uint64_t g_lastBytes = 0, g_lastTicks = 0;
static void setStats(HWND hwnd, const StreamStats& s, bool running) {
    wchar_t buf[512];
    if (running) {
        uint64_t now = GetTickCount64();
        double mbps = 0.0;
        if (g_lastTicks) {
            uint64_t dt = now - g_lastTicks;
            if (dt > 0) mbps = (double)(s.bytesSent - g_lastBytes) * 8.0 / dt / 1000.0;
        }
        g_lastBytes = s.bytesSent; g_lastTicks = now;
        // realtime send rate (KB/s): bytesSent is cumulative, delta / seconds = rate
        static uint64_t lastBytes = 0, lastT = 0;
        double kbpsSend = 0.0;
        if (lastT) {
            uint64_t dt = now - lastT;
            if (dt >= 250) kbpsSend = (double)(s.bytesSent - lastBytes) / (double)dt * 1000.0 / 1024.0;
        }
        lastBytes = s.bytesSent; lastT = now;
        int targetKbps = g_app.streamer.currentBitrateKbps();
        swprintf_s(buf, L"采集 %llu | 发送 %llu | 差分跳过 %llu   码率 %.2f Mbps | 发送 %.0f KB/s | 目标 %d kbps | 丢帧 %llu | %llu 包 | %ls",
                   (unsigned long long)s.framesCaptured, (unsigned long long)s.framesSent,
                   (unsigned long long)s.framesSkipped, mbps, kbpsSend, targetKbps,
                   (unsigned long long)s.sendDropFrames, (unsigned long long)s.packetsSent,
                   s.gdiMode ? L"GDI捕获" : L"DXGI捕获");
    } else {
        g_lastTicks = 0;
        if (btnChecked(g_app.hMc)) {
            wchar_t grp[64] = { 0 };
            GetWindowTextW(g_app.hIpEdit, grp, 63);
            swprintf_s(buf, L"未启动 - 组播模式: 向组 %s 推流(支持 40 客户端, 组地址可改)", grp);
        } else {
            swprintf_s(buf, L"未启动 - 输入客户端 IP 后点击 开始推流");
        }
    }
    SetWindowTextW(hwnd, buf);
}

static void updateStartButton() {
    SetWindowTextW(g_app.hStart, g_app.started ? L"停止推流" : L"开始推流");
    // repaint NOW: the owner-draw button draws the current text on demand, so without forcing
    // a synchronous redraw the OLD label stays on screen until the next natural invalidation --
    // the desktop capture could then mix the old and new text into one video frame (字体重叠).
    InvalidateRect(g_app.hStart, nullptr, TRUE);
    UpdateWindow(g_app.hStart);
}

// multicast-mode switch: the edit box toggles semantics between "client IP" and "multicast group address" (both editable)
static void updateMcControls() {
    bool mc = btnChecked(g_app.hMc);
    if (mc) {
        wchar_t ip[64] = { 0 };
        GetWindowTextW(g_app.hIpEdit, ip, 63);
        if (ip[0] == 0)
            SetWindowTextW(g_app.hIpEdit, kDefaultGroup);   // fill in the multicast group when empty
    }
    SetWindowTextW(g_app.hStats, L"");
}

static void startStreaming() {
    // guard against fast double-click/re-entry
    static std::atomic<bool> s_busy{ false };
    if (s_busy.exchange(true)) return;  // already in the start process, ignore duplicate calls
    if (g_app.started) { s_busy.store(false); return; }  // already running

    g_app.streamer.setAdaptiveBitrate(btnChecked(g_app.hAdaptive));
    bool mc = btnChecked(g_app.hMc);
    wchar_t ip[64] = { 0 };
    GetWindowTextW(g_app.hIpEdit, ip, 63);
    if (!ip[0]) {
        if (mc) wcscpy_s(ip, kDefaultGroup);
        else    autoDetectIP(ip, 64);
        SetWindowTextW(g_app.hIpEdit, ip);  // echo back the auto-detected result
    }
    g_app.targetIP = ip;
    // convert the wide-char IP to narrow (ASCII-compatible), pass to Streamer
    char ipA[64] = { 0 };
    WideCharToMultiByte(CP_UTF8, 0, ip, -1, ipA, sizeof(ipA), nullptr, nullptr);
    g_app.streamer.setQuality(qualityFromIndex(comboCurSel(g_app.hQuality)));
    g_app.streamer.setDifferentialEnabled(btnChecked(g_app.hDiff));
    g_app.preview.showDirty.store(btnChecked(g_app.hDirty));
    // exclude ONLY the preview pane: the part of the teacher window that mirrors the desktop live. Sending
    // that region would recurse into an infinite picture-in-picture mirror; the rest of the window (title
    // bar, buttons, client list) IS sent so students can see the teacher window's interface -- the user asked
    // for exactly this ("学生端能看到教师窗口界面, 预览小窗保持深色"). Set before start so the first frames are clean too.
    RECT pr = previewScreenRect();
    g_app.streamer.setSelfExcludeRect(pr, true);
    if (g_app.streamer.start(ipA, 5004, 5005)) {
        g_app.started = true;
        updateStartButton();
        StreamStats s = g_app.streamer.snapshot();
        setStats(g_app.hStats, s, true);
        s_busy.store(false);
    } else {
        g_app.streamer.setSelfExcludeRect(RECT{ 0, 0, 0, 0 }, false);   // revert on start failure
        MessageBoxW(g_app.hWnd, L"推流启动失败(检查 IP 与网络)", L"screen_cast", MB_ICONERROR);
        s_busy.store(false);
    }
}

static void stopStreaming() {
    g_app.streamer.setSelfExcludeRect(RECT{ 0, 0, 0, 0 }, false);
    g_app.streamer.stop();
    g_app.started = false;
    updateStartButton();
    StreamStats s = g_app.streamer.snapshot();
    setStats(g_app.hStats, s, false);   // show "not started" immediately, don't wait for the throttle cycle
}

// client-list panel: right side, sits inside the rounded card painted by paintPreview()
static void layoutClientPanel(HWND hwnd) {
    RECT rc; GetClientRect(hwnd, &rc);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    int x = w - 194, pw = 186;
    SetWindowPos(g_app.hClients, nullptr, x + 20, 80, pw - 20, 20, SWP_NOZORDER | SWP_NOACTIVATE);   // +20: clear of the accent bar
    SetWindowPos(g_app.hClientLb, nullptr, x, 102, pw, (h > 118 ? h - 110 : 100), SWP_NOZORDER | SWP_NOACTIVATE);
}

// refresh the client list: header shows the count, list shows each IP (rebuild only on content change, anti-flicker)
static void refreshClientList() {
    std::vector<std::string> ips = g_app.streamer.clientIps();
    wchar_t head[64];
    swprintf_s(head, L"客户端 (%zu)", ips.size());
    SetWindowTextW(g_app.hClients, head);
    if (ips == g_app.lastClients) return;
    g_app.lastClients = ips;
    SendMessageW(g_app.hClientLb, LB_RESETCONTENT, 0, 0);
    for (const auto& ip : ips) {
        wchar_t wip[64] = { 0 };
        MultiByteToWideChar(CP_UTF8, 0, ip.c_str(), -1, wip, 63);
        SendMessageW(g_app.hClientLb, LB_ADDSTRING, 0, (LPARAM)wip);
    }
}

// preview area: below the top bar (labels 5-17, controls 16-48, stats 50-70), right side yields to the client-list card
static RECT previewRect() {
    RECT rc; GetClientRect(g_app.hWnd, &rc);
    RECT pr = rc;
    pr.top = 74;
    pr.right = rc.right - 212;   // 8px breathing room before the client-list card (left = w-204)
    if (pr.top >= pr.bottom) pr.top = pr.bottom - 1;
    if (pr.right <= pr.left) pr.right = pr.left + 1;
    return pr;
}

// rebuild the off-screen back buffer to match the client-area size
static void ensureBackbuffer(HWND hwnd) {
    RECT rc; GetClientRect(hwnd, &rc);
    int w = rc.right - rc.left, h = rc.bottom - rc.top;
    if (g_app.memDc && g_app.memW == w && g_app.memH == h) return;
    if (g_app.memDc) {
        SelectObject(g_app.memDc, g_app.memOld);
        DeleteObject(g_app.memBmp);
        DeleteDC(g_app.memDc);
        g_app.memDc = nullptr; g_app.memBmp = nullptr; g_app.memOld = nullptr;
    }
    HDC hdc = GetDC(hwnd);
    g_app.memDc = CreateCompatibleDC(hdc);
    g_app.memBmp = CreateCompatibleBitmap(hdc, w, h);
    ReleaseDC(hwnd, hdc);
    if (g_app.memDc && g_app.memBmp)
        g_app.memOld = (HGDIOBJ)SelectObject(g_app.memDc, g_app.memBmp);
    g_app.memW = w; g_app.memH = h;
}

// double-buffered paint: draw everything off-screen, commit in one BitBlt, eliminating intermediate-state flicker
static void paintPreview(HWND hwnd) {
    PAINTSTRUCT ps;
    HDC hdc = BeginPaint(hwnd, &ps);
    ensureBackbuffer(hwnd);
    if (!g_app.memDc || !g_app.memBmp) { EndPaint(hwnd, &ps); return; }
    RECT rc; GetClientRect(hwnd, &rc);
    // dark base
    HBRUSH bg = CreateSolidBrush(kBg);
    FillRect(g_app.memDc, &rc, bg);
    DeleteObject(bg);
    // preview pane: rounded dark panel + rounded-corner video (2026 player card look)
    RECT pr = previewRect();
    const int rad = 12;
    mu::FillRound(g_app.memDc, pr, rad, kPanel);
    if (g_app.preview.w > 0) {
        RECT inner = pr;
        inner.left += 1; inner.top += 1; inner.right -= 1; inner.bottom -= 1;
        // clip the picture to the rounded corners so it looks like a video player
        HRGN clip = CreateRoundRectRgn(inner.left, inner.top, inner.right + 1, inner.bottom + 1, rad * 2, rad * 2);
        int saved = SaveDC(g_app.memDc);
        SelectClipRgn(g_app.memDc, clip);
        Preview_Draw(g_app.preview, g_app.memDc, inner);
        RestoreDC(g_app.memDc, saved);
        DeleteObject(clip);
    } else {
        RECT tr = pr;
        SetTextColor(g_app.memDc, kDim); SetBkMode(g_app.memDc, TRANSPARENT);
        DrawTextW(g_app.memDc, L"等待画面...(启动推流后显示桌面预览)", -1, &tr, DT_CENTER | DT_VCENTER | DT_SINGLELINE);
    }
    // preview border: neon cyan "live" ring while streaming, soft gray when idle
    if (g_app.started)
        mu::StrokeRound(g_app.memDc, pr, rad, mu::kNeonA1, 1);
    else
        mu::StrokeRound(g_app.memDc, pr, rad, mu::kBorder, 1);
    // LIVE badge (preview top-right corner, pulsing white dot) while streaming
    if (g_app.started) {
        int bw = 60, bh = 20;
        RECT b{ pr.right - bw - 8, pr.top + 8, pr.right - 8, pr.top + 8 + bh };
        mu::FillRound(g_app.memDc, b, bh / 2, RGB(0xD8, 0x3A, 0x56));
        mu::StrokeRound(g_app.memDc, b, bh / 2, RGB(0xF0, 0x6A, 0x84), 1);
        uint64_t t = GetTickCount64() / 250 % 3;   // pulsing dot
        int dsz = 5 + (int)t;
        HBRUSH dot = CreateSolidBrush(RGB(0xFF, 0xFF, 0xFF));
        HGDIOBJ od = SelectObject(g_app.memDc, dot);
        HGDIOBJ onp = SelectObject(g_app.memDc, GetStockObject(NULL_PEN));
        Ellipse(g_app.memDc, b.left + 13 - dsz / 2, b.top + bh / 2 - dsz / 2,
                b.left + 13 + dsz / 2, b.top + bh / 2 + dsz / 2);
        SelectObject(g_app.memDc, od); DeleteObject(dot);
        SelectObject(g_app.memDc, onp);
        RECT tr{ b.left + 24, b.top, b.right - 4, b.bottom };
        mu::DrawLabel(g_app.memDc, tr, L"LIVE", RGB(0xFF, 0xFF, 0xFF), g_app.font,
                      DT_LEFT | DT_VCENTER | DT_SINGLELINE);
    }
    // client-list card (right side, rounded panel + cyan accent bar)
    {
        RECT cr{ rc.right - 204, 72, rc.right - 8, rc.bottom - 8 };
        mu::FillRound(g_app.memDc, cr, 12, kPanel);
        mu::StrokeRound(g_app.memDc, cr, 12, mu::kBorder, 1);
        // cyan section marker beside the client header (same height as the header row; the
        // listbox below is opaque so the marker must stay in the header band)
        RECT ab{ cr.left + 12, cr.top + 10, cr.left + 15, cr.top + 28 };
        mu::FillRound(g_app.memDc, ab, 2, mu::kNeonA1);
    }
    BitBlt(hdc, 0, 0, rc.right - rc.left, rc.bottom - rc.top, g_app.memDc, 0, 0, SRCCOPY);
    EndPaint(hwnd, &ps);
}

static LRESULT CALLBACK wndProc(HWND hwnd, UINT msg, WPARAM wp, LPARAM lp) {
    switch (msg) {
    case WM_CREATE: {
        g_app.bgBrush    = CreateSolidBrush(kBg);
        g_app.panelBrush = CreateSolidBrush(kPanel);
        g_app.font = CreateFontW(-16, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                                 OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY, DEFAULT_PITCH, L"Segoe UI");

        // top bar: labels + IP/multicast group + quality + differential switch + changed-region + multicast mode + start button
        g_app.hLblIp = CreateWindowW(L"STATIC", L"教师IP", WS_CHILD | WS_VISIBLE,
                                     10, 5, 52, 12, hwnd, (HMENU)IDC_LBL_IP, GetModuleHandleW(nullptr), nullptr);
        g_app.hIpEdit = CreateWindowW(L"EDIT", g_app.targetIP.c_str(), WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                                      66, 16, 94, 28, hwnd, (HMENU)IDC_IP_EDIT, GetModuleHandleW(nullptr), nullptr);
        g_app.hLblQ = CreateWindowW(L"STATIC", L"画质", WS_CHILD | WS_VISIBLE,
                                    164, 5, 30, 12, hwnd, (HMENU)IDC_LBL_Q, GetModuleHandleW(nullptr), nullptr);
        g_app.hQuality = CreateWindowW(L"COMBOBOX", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | CBS_DROPDOWNLIST | CBS_OWNERDRAWFIXED | CBS_HASSTRINGS,
                                       198, 14, 96, 120, hwnd, (HMENU)IDC_QUALITY, GetModuleHandleW(nullptr), nullptr);
        comboAdd(g_app.hQuality, L"原始分辨率");
        comboAdd(g_app.hQuality, L"高清 1080p");
        comboSetSel(g_app.hQuality, 0);
        g_app.hDiff = CreateWindowW(L"BUTTON", L"差分投屏", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | BS_OWNERDRAW | BS_NOTIFY,
                                    300, 16, 84, 24, hwnd, (HMENU)IDC_DIFF, GetModuleHandleW(nullptr), nullptr);
        btnSetCheck(g_app.hDiff, true);
        g_app.hDirty = CreateWindowW(L"BUTTON", L"变化区域", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | BS_OWNERDRAW | BS_NOTIFY,
                                     388, 16, 84, 24, hwnd, (HMENU)IDC_DIRTYS, GetModuleHandleW(nullptr), nullptr);
        g_app.hMc = CreateWindowW(L"BUTTON", L"组播模式", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | BS_OWNERDRAW | BS_NOTIFY,
                                  476, 16, 88, 24, hwnd, (HMENU)IDC_MCAST, GetModuleHandleW(nullptr), nullptr);
        btnSetCheck(g_app.hMc, true);   // multicast checked by default
        g_app.hAdaptive = CreateWindowW(L"BUTTON", L"智能码率", WS_CHILD | WS_VISIBLE | BS_AUTOCHECKBOX | BS_OWNERDRAW | BS_NOTIFY,
                                        568, 16, 84, 24, hwnd, (HMENU)IDC_ADAPTIVE, GetModuleHandleW(nullptr), nullptr);
        btnSetCheck(g_app.hAdaptive, false);   // adaptive bitrate off by default (uses preset/manual bitrate)
        g_app.hStart = CreateWindowW(L"BUTTON", L"开始推流", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | BS_NOTIFY,
                                     660, 14, 104, 30, hwnd, (HMENU)IDC_BTN_START, GetModuleHandleW(nullptr), nullptr);
        // multicast mode defaults to the multicast address
        SetWindowTextW(g_app.hIpEdit, kDefaultGroup);
        mu::MuStyleEdit(g_app.hIpEdit);   // dark rounded border instead of the system light one

        g_app.hStats = CreateWindowW(L"STATIC", L"未启动", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                     10, 50, 740, 20, hwnd, (HMENU)IDC_STATS, GetModuleHandleW(nullptr), nullptr);

        // client-list panel (right side; layoutClientPanel positions it inside the rounded card)
        g_app.hClients = CreateWindowW(L"STATIC", L"客户端 (0)", WS_CHILD | WS_VISIBLE | SS_LEFT,
                                       0, 0, 186, 20, hwnd, (HMENU)IDC_CLIENTS, GetModuleHandleW(nullptr), nullptr);
        g_app.hClientLb = CreateWindowW(L"LISTBOX", L"", WS_CHILD | WS_VISIBLE | WS_VSCROLL | LBS_NOTIFY | LBS_OWNERDRAWFIXED | LBS_HASSTRINGS | LBS_NOINTEGRALHEIGHT,
                                        0, 0, 186, 280, hwnd, (HMENU)IDC_CLIENT_LB, GetModuleHandleW(nullptr), nullptr);
        SendMessageW(g_app.hClients, WM_SETFONT, (WPARAM)g_app.font, TRUE);
        SendMessageW(g_app.hClientLb, WM_SETFONT, (WPARAM)g_app.font, TRUE);

        for (HWND h : { g_app.hLblIp, g_app.hLblQ, g_app.hIpEdit, g_app.hQuality, g_app.hDiff, g_app.hDirty, g_app.hMc, g_app.hAdaptive, g_app.hStart, g_app.hStats })
            SendMessageW(h, WM_SETFONT, (WPARAM)g_app.font, TRUE);

        layoutClientPanel(hwnd);
        SetTimer(hwnd, g_app.timer, 33, nullptr);   // 30fps 刷新预览+统计
        return 0;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == IDC_BTN_START) {
            g_app.started ? stopStreaming() : startStreaming();
        } else if (id == IDC_QUALITY && HIWORD(wp) == CBN_SELCHANGE) {
            g_app.streamer.setQuality(qualityFromIndex(comboCurSel(g_app.hQuality)));
            SetWindowTextW(g_app.hStats, L"清晰度已切换, 等待生效...");
        } else if (id == IDC_DIFF) {
            g_app.streamer.setDifferentialEnabled(btnChecked(g_app.hDiff));
        } else if (id == IDC_DIRTYS) {
            g_app.preview.showDirty.store(btnChecked(g_app.hDirty));
        } else if (id == IDC_MCAST) {
            updateMcControls();
        } else if (id == IDC_ADAPTIVE) {
            // mid-stream toggle: the capture thread detects the effective-bitrate change and rebuilds the
            // encoder (forcing a fresh keyframe); when switching OFF it restores the manual/preset rate
            g_app.streamer.setAdaptiveBitrate(btnChecked(g_app.hAdaptive));
            SetWindowTextW(g_app.hStats, L"智能码率已切换, 等待生效...");
        }
        return 0;
    }
    case WM_TIMER: {
        // if the stream thread exits unexpectedly (capture/encode init failure), auto-reset the UI so the user can click "start" to retry
        if (g_app.started && !g_app.streamer.isRunning()) stopStreaming();
        // throttle the status-bar text to 2Hz, avoiding the STATIC control's 30fps redraw flicker
        uint64_t now = GetTickCount64();
        if (now - g_app.lastStatsMs >= 500) {
            g_app.lastStatsMs = now;
            StreamStats s = g_app.streamer.snapshot();
            setStats(g_app.hStats, s, g_app.started);
            refreshClientList();   // client count + IPs (rebuild only on content change)
        }
        // preview area: redraw only when there's a new frame and >= 66ms (about 15fps) since the last draw, invalidate only the preview area (not the controls)
        uint64_t seq = g_app.preview.seq.load();
        if (seq != g_app.lastPreviewSeq && now - g_app.lastPreviewMs >= 66) {
            g_app.lastPreviewSeq = seq;
            g_app.lastPreviewMs = now;
            RECT pr = previewRect();
            InvalidateRect(hwnd, &pr, FALSE);
        }
        return 0;
    }
    case WM_SIZE:
        layoutClientPanel(hwnd);
        ensureBackbuffer(hwnd);
        InvalidateRect(hwnd, nullptr, FALSE);
        return 0;
    case WM_PAINT:
        paintPreview(hwnd);
        return 0;
    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wp;
        int id = GetDlgCtrlID((HWND)lp);
        if (id == IDC_STATS) {            // status band keeps its panel chip
            SetTextColor(hdc, kText);
            SetBkColor(hdc, kPanel);
            return (LRESULT)g_app.panelBrush;
        }
        if (id == IDC_CLIENTS) {          // client header: kPanel chip (transparent statics can paint white here;
            SetTextColor(hdc, kDim);      // the section marker at card.left+12..+15 sits LEFT of the text, no overlap)
            SetBkColor(hdc, kPanel);
            return (LRESULT)g_app.panelBrush;
        }
        // top-bar labels: transparent over the window bg
        SetTextColor(hdc, mu::kTextFaint);
        SetBkColor(hdc, kBg);
        SetBkMode(hdc, TRANSPARENT);
        return (LRESULT)GetStockObject(NULL_BRUSH);
    }
    case WM_CTLCOLOREDIT:
    case WM_CTLCOLORBTN:
    case WM_CTLCOLORLISTBOX: {
        HDC hdc = (HDC)wp;
        SetTextColor(hdc, kText);
        SetBkColor(hdc, kPanel);
        return (LRESULT)g_app.panelBrush;
    }
    case WM_MEASUREITEM: {
        // owner-draw combo + listbox: uniform row height
        MEASUREITEMSTRUCT* mi = (MEASUREITEMSTRUCT*)lp;
        if (mi->CtlType == ODT_COMBOBOX || mi->CtlType == ODT_LISTBOX) mi->itemHeight = 26;
        return TRUE;
    }
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT* di = (DRAWITEMSTRUCT*)lp;
        if (!di) return FALSE;
        // push button (start/stop) -- accent gradient; checkboxes drawn via DrawCheckbox
        if (di->CtlType == ODT_BUTTON) {
            HWND h = di->hwndItem;
            // NOTE: 不能用 (style & BS_AUTOCHECKBOX) 判断 —— BS_OWNERDRAW(0xB) 的低位含 BS_AUTOCHECKBOX(0x3),
            // 两者 OR 后同值, 位掩码无法区分; 必须按控件 ID 判别
            int cid = (int)di->CtlID;
            bool isCheck = (cid == IDC_DIFF || cid == IDC_DIRTYS || cid == IDC_MCAST || cid == IDC_ADAPTIVE);
            bool hover = mu::MuIsHover(h);
            bool pressed = (di->itemState & ODS_SELECTED) != 0;
            if (isCheck) {
                bool checked = SendMessageW(h, BM_GETCHECK, 0, 0) == BST_CHECKED;
                wchar_t txt[64]; GetWindowTextW(h, txt, 64);
                mu::DrawCheckbox(di->hDC, di->rcItem, txt, checked, hover, false /*cyan gradient*/, g_app.font);
            } else {
                wchar_t txt[64]; GetWindowTextW(h, txt, 64);
                bool accent = ((int)di->CtlID == IDC_BTN_START);
                // start/stop button carries a vector icon: play while idle, stop while streaming
                mu::MuIcon icon = accent ? (g_app.started ? mu::kIconStop : mu::kIconPlay) : mu::kIconNone;
                mu::DrawButton(di->hDC, di->rcItem, txt, hover, pressed, accent,
                               false /*cyan gradient for accent*/, g_app.font, icon);
            }
            return TRUE;
        }
        // combo item / list item
        if (di->CtlType == ODT_COMBOBOX || di->CtlType == ODT_LISTBOX) {
            int idx = (int)di->itemID;
            if (idx >= 0) {
                wchar_t txt[128] = {};
                if (di->CtlType == ODT_COMBOBOX) SendMessageW(di->hwndItem, CB_GETLBTEXT, idx, (LPARAM)txt);
                else                             SendMessageW(di->hwndItem, LB_GETTEXT, idx, (LPARAM)txt);
                bool sel = (di->itemState & ODS_SELECTED) != 0;
                bool hot = (di->itemState & ODS_HOTLIGHT) != 0;
                mu::DrawListItem(di->hDC, di->rcItem, txt, sel, hot, false /*cyan bar*/, g_app.font);
            } else if (di->CtlType == ODT_COMBOBOX) {
                // closed combo face (itemID == -1): dark field + selected item text + caret
                RECT rc = di->rcItem;
                HBRUSH br = CreateSolidBrush(mu::kInput);
                FillRect(di->hDC, &rc, br);
                DeleteObject(br);
                mu::StrokeRound(di->hDC, rc, 6, mu::kBorder, 1);
                int sel = (int)SendMessageW(di->hwndItem, CB_GETCURSEL, 0, 0);
                wchar_t txt[128] = {};
                if (sel >= 0) SendMessageW(di->hwndItem, CB_GETLBTEXT, sel, (LPARAM)txt);
                RECT tr = rc; tr.left += 10; tr.right -= 22;
                mu::DrawLabel(di->hDC, tr, txt, mu::kTextMain, g_app.font, DT_LEFT | DT_VCENTER | DT_SINGLELINE | DT_END_ELLIPSIS);
                // drop-down caret (small triangle)
                POINT pts[3] = { { rc.right - 15, rc.top + (rc.bottom - rc.top) / 2 - 3 },
                                 { rc.right - 8, rc.top + (rc.bottom - rc.top) / 2 - 3 },
                                 { rc.right - 11, rc.top + (rc.bottom - rc.top) / 2 + 3 } };
                HBRUSH cb = CreateSolidBrush(mu::kTextDim);
                HPEN cp = CreatePen(PS_SOLID, 1, mu::kTextDim);
                HGDIOBJ ob = SelectObject(di->hDC, cb);
                HGDIOBJ op = SelectObject(di->hDC, cp);
                Polygon(di->hDC, pts, 3);
                SelectObject(di->hDC, ob); DeleteObject(cb);
                SelectObject(di->hDC, op); DeleteObject(cp);
            }
            return TRUE;
        }
        return FALSE;
    }
    case WM_MOUSEMOVE: {
        mu::MuTrackHover(hwnd, lp);
        return 0;
    }
    case WM_MOUSELEAVE:
        mu::MuClearHover(hwnd);
        return 0;
    case WM_ERASEBKGND:
        return 1;   // 由 paintPreview 填充, 防闪烁
    case WM_DESTROY:
        KillTimer(hwnd, g_app.timer);
        try { if (g_app.started) stopStreaming(); } catch (...) {}
        if (g_app.font)      { DeleteObject(g_app.font); g_app.font = nullptr; }
        if (g_app.bgBrush)   { DeleteObject(g_app.bgBrush); g_app.bgBrush = nullptr; }
        if (g_app.panelBrush){ DeleteObject(g_app.panelBrush); g_app.panelBrush = nullptr; }
        if (g_app.memDc) { SelectObject(g_app.memDc, g_app.memOld); DeleteObject(g_app.memBmp); DeleteDC(g_app.memDc); }
        PostQuitMessage(0);
        return 0;
    case WM_WINDOWPOSCHANGED:
        if (g_app.started) {   // keep the preview-pane rect in sync while the window moves/resizes (excluded region follows it)
            RECT pr = previewScreenRect();
            g_app.streamer.setSelfExcludeRect(pr, true);
        }
        return 0;
    default:
        return DefWindowProcW(hwnd, msg, wp, lp);
    }
}

int WINAPI wWinMain(HINSTANCE hInst, HINSTANCE, PWSTR, int) {
    // DPI awareness: without it Windows DPI-virtualizes this process, so GetClientRect/ClientToScreen return
    // logical coords while DXGI capture works in physical pixels -> the preview-pane exclusion rect would be
    // misplaced on scaled (non-100%) displays. Must be called before any window is created.
    SetProcessDPIAware();
    SetUnhandledExceptionFilter(crashHandler);
    int argc = 0;
    LPWSTR* wargv = CommandLineToArgvW(GetCommandLineW(), &argc);
    if (argc > 1) g_app.targetIP = wargv[1];
    else {
        wchar_t autoip[64] = {}; autoDetectIP(autoip, 64); g_app.targetIP = autoip;
    }   // no args = auto-detect the local IP
    if (wargv) LocalFree(wargv);

    WNDCLASSW wc = {};
    wc.lpfnWndProc = wndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hIcon = LoadIconW(hInst, L"APPICON");                    // custom embedded icon (msi/teacher.ico)
    wc.hbrBackground = g_app.bgBrush;                            // set by WM_CREATE (WM_ERASEBKGND=1, paint fills fully)
    wc.lpszClassName = L"ScreenCastTeacher";
    RegisterClassW(&wc);

    HWND hwnd = CreateWindowExW(0, L"ScreenCastTeacher", L"screen_cast - 教师端 v" SCREENCAST_VERSION_WS,
                              WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU | WS_MINIMIZEBOX | WS_CLIPCHILDREN, CW_USEDEFAULT, CW_USEDEFAULT, 790, 480,
                              nullptr, nullptr, hInst, nullptr);
    if (!hwnd) return 1;
    g_app.hWnd = hwnd;
    mu::ApplyDarkFrame(hwnd, true);   // dark title bar + Win11 rounded corners (2026 look)
    ShowWindow(hwnd, SW_SHOW);
    UpdateWindow(hwnd);

    // register the preview callback: when the capture thread produces a new frame, scale-copy it into the preview buffer (preview area displays the desktop in real time)
    g_app.streamer.setPreviewCallback([](const uint8_t* bgra, int w, int h, int stride,
                                         const RECT* dirty, int n) {
        Preview_Update(g_app.preview, bgra, w, h, stride, dirty, n);
    });

    startStreaming();   // start streaming immediately on open (default 127.0.0.1; for multicast check the box first)

    MSG msg;
    while (GetMessageW(&msg, nullptr, 0, 0) > 0) {
        TranslateMessage(&msg);
        DispatchMessageW(&msg);
    }
    return (int)msg.wParam;
}
