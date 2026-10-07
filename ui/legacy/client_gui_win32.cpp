// client_gui.cpp - client window version: FFmpeg decode + D3D11 render
// Wide-char Chinese title bar with live stats: teacher IP / FPS / loss% / latency / queue
// usage: client_gui.exe <teacherIP> [multicastGroup]
//        with a multicastGroup (e.g. 239.255.1.1) it receives via multicast; omitted = unicast
#include <winsock2.h>
#include <windows.h>
#include <dbghelp.h>
#pragma comment(lib, "dbghelp.lib")
// GUI subsystem (no black console window), but the entry stays main(): explicitly set the CRT entry, bypassing linker WinMain search
#pragma comment(linker, "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup")
#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstring>
#include <cwchar>
#include <mutex>
#include <thread>
#include <vector>
#include "net/client_receiver.h"
#include "decode/ffmpeg_decoder.h"
#include "render/d3d11_renderer.h"
#include "ui/modern_ui.h"
#include "version.h"

// crash dump: writes a minidump on an unhandled exception for diagnostics
static LONG WINAPI clientCrashHandler(EXCEPTION_POINTERS* ep) {
    HANDLE hFile = CreateFileW(L"client_crash.dmp", GENERIC_WRITE, 0, nullptr,
                               CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (hFile != INVALID_HANDLE_VALUE) {
        MINIDUMP_EXCEPTION_INFORMATION mei{};
        mei.ThreadId = GetCurrentThreadId();
        mei.ExceptionPointers = ep;
        mei.ClientPointers = FALSE;
        MiniDumpWriteDump(GetCurrentProcess(), GetCurrentProcessId(), hFile,
                          MiniDumpNormal, &mei, nullptr, nullptr);
        CloseHandle(hFile);
    }
    FILE* f = fopen("client_crash.txt", "w");
    if (f) {
        fprintf(f, "code=0x%X addr=%p\n",
                ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress);
        fclose(f);
    }
    return EXCEPTION_EXECUTE_HANDLER;
}

constexpr uint32_t kLatUnavailable = 0xFFFFFFFF;   // clocks not synced (cross machine), latency shows "--"

struct FrameQueue {
    struct Frame { std::vector<uint8_t> data; int w = 0, h = 0; uint64_t pts = 0; };
    std::mutex mtx;
    std::vector<Frame> frames;   // decoded BGRA, each frame carries its OWN resolution
    void push(const uint8_t* bgra, int fw, int fh, uint64_t ms) {
        std::lock_guard<std::mutex> lk(mtx);
        if (frames.size() > 3) frames.erase(frames.begin());   // drop backlog, keep realtime
        // keep the frame's own w/h: resolution switches mid-stream must not corrupt the size reported for
        // an older frame that is still in the queue (mismatch -> renderer would read past the buffer end)
        Frame f;
        f.data.assign(bgra, bgra + (size_t)fw * fh * 4);
        f.w = fw; f.h = fh; f.pts = ms;
        frames.push_back(std::move(f));
    }
    bool pop(std::vector<uint8_t>& out, int& fw, int& fh, uint64_t& ms) {
        std::lock_guard<std::mutex> lk(mtx);
        if (frames.empty()) return false;
        out = std::move(frames.front().data);
        fw = frames.front().w; fh = frames.front().h;
        ms = frames.front().pts;
        frames.erase(frames.begin());
        return true;
    }
    // peek the front frame's timestamp (doesn't take data, doesn't copy), so the main loop can pace consumption at 33ms
    bool peekMs(uint64_t* ms) {
        std::lock_guard<std::mutex> lk(mtx);
        if (frames.empty()) return false;
        *ms = frames.front().pts;
        return true;
    }
    int sizeSafe() {
        std::lock_guard<std::mutex> lk(mtx);
        return (int)frames.size();
    }
};

// encoded-frame queue: the receive thread only "copies and enqueues" then returns immediately, decoding is moved to a dedicated
// decode thread, so network receive/reassembly is no longer dragged by CPU decoding (UDP buffer not filled -> no drops -> no keyframe storm)
struct EncodedQueue {
    struct Item { std::vector<uint8_t> data; uint64_t captureMs; };
    std::mutex mtx;
    std::vector<Item> items;
    void push(Item it) {
        std::lock_guard<std::mutex> lk(mtx);
        if (items.size() > 8) items.erase(items.begin());   // cap 8 frames, drop the old ones past the limit to stay realtime
        items.push_back(std::move(it));
    }
    bool pop(Item& out) {
        std::lock_guard<std::mutex> lk(mtx);
        if (items.empty()) return false;
        out = std::move(items.front()); items.erase(items.begin());
        return true;
    }
};

static std::atomic<bool> g_running{ true };

// persist the last-configured IP and multicast group: saved to a text file next to the exe
static void saveConfig(const wchar_t* ip, const wchar_t* group) {
    wchar_t path[MAX_PATH]; GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t* slash = wcsrchr(path, L'\\'); if (slash) *(slash + 1) = L'\0';
    wcscat_s(path, L"client_config.txt");
    FILE* f = _wfopen(path, L"w,ccs=UTF-8");
    if (f) { fwprintf(f, L"%s\n%s\n", ip, group); fclose(f); }
}
static void loadConfig(wchar_t* ipOut, wchar_t* groupOut, size_t cap) {
    wchar_t path[MAX_PATH]; GetModuleFileNameW(nullptr, path, MAX_PATH);
    wchar_t* slash = wcsrchr(path, L'\\'); if (slash) *(slash + 1) = L'\0';
    wcscat_s(path, L"client_config.txt");
    FILE* f = _wfopen(path, L"r,ccs=UTF-8");
    if (f) {
        fgetws(ipOut, (int)cap, f);
        fgetws(groupOut, (int)cap, f);
        fclose(f);
        // strip newline characters
        wchar_t* arr[2] = { ipOut, groupOut };
        for (int i = 0; i < 2; ++i) {
            wchar_t* p = arr[i];
            while (*p && *p != L'\n' && *p != L'\r') ++p;
            *p = L'\0';
        }
    }
    if (!ipOut[0])  wcscpy_s(ipOut, cap, L"192.168.110.159");
    if (!groupOut[0]) wcscpy_s(groupOut, cap, L"239.255.1.1");
}

// ---------- connection dialog: shown on double-click launch (no command-line args), enter the teacher IP ----------
struct ConnDialog {
    HWND hwnd = nullptr, ipEdit = nullptr, groupEdit = nullptr;
    bool done = false, ok = false;
    wchar_t ip[64] = L"", group[64] = L"239.255.1.1";
    HFONT font = nullptr;
    HBRUSH bgBrush = nullptr, panelBrush = nullptr;
};

static LRESULT CALLBACK connWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    ConnDialog* d = (ConnDialog*)GetWindowLongPtrW(h, GWLP_USERDATA);
    switch (msg) {
    case WM_NCCREATE: {
        // DefWindowProc MUST process WM_NCCREATE: it stores the window title text (CreateWindowEx's
        // title param). Returning TRUE without calling it leaves the title bar EMPTY (observed: both
        // dialogs had blank titles). Then stash the dialog struct for later messages.
        CREATESTRUCTW* cs = (CREATESTRUCTW*)lp;
        LRESULT r = DefWindowProcW(h, msg, wp, lp);
        if (r) SetWindowLongPtrW(h, GWLP_USERDATA, (LONG_PTR)cs->lpCreateParams);
        return r;
    }
    case WM_COMMAND: {
        int id = LOWORD(wp);
        if (id == 1 || id == 2) {
            if (id == 1) {
                GetWindowTextW(d->ipEdit, d->ip, 63);
                GetWindowTextW(d->groupEdit, d->group, 63);
                if (d->ip[0] == L'\0') {
                    MessageBoxW(h, L"请输入教师端 IP", L"连接教师端", MB_ICONWARNING);
                    return 0;   // stay in the dialog
                }
                d->ok = true;
            }
            d->done = true;
            DestroyWindow(h);
        }
        return 0;
    }
    case WM_ERASEBKGND: {
        // subtle vertical gradient: lighter at the top (elevated look), deep at the bottom
        RECT rc; GetClientRect(h, &rc);
        mu::GradientFillV((HDC)wp, rc, RGB(0x28, 0x2D, 0x47), mu::kBgDark, 20);
        return 1;
    }
    case WM_CTLCOLORSTATIC: {
        HDC hdc = (HDC)wp;
        SetTextColor(hdc, mu::kTextDim);
        SetBkMode(hdc, TRANSPARENT);
        return (LRESULT)GetStockObject(NULL_BRUSH);   // transparent over the gradient bg
    }
    case WM_CTLCOLOREDIT: {
        HDC hdc = (HDC)wp;
        SetTextColor(hdc, mu::kTextMain);
        SetBkColor(hdc, mu::kInput);
        return (LRESULT)d->panelBrush;
    }
    case WM_DRAWITEM: {
        DRAWITEMSTRUCT* di = (DRAWITEMSTRUCT*)lp;
        if (!di) return FALSE;
        bool hover = mu::MuIsHover(di->hwndItem);
        bool pressed = (di->itemState & ODS_SELECTED) != 0;
        wchar_t txt[64]; GetWindowTextW(di->hwndItem, txt, 64);
        bool accent = ((int)di->CtlID == 1);   // 连接 = accent, 取消 = ghost
        mu::MuIcon icon = accent ? mu::kIconLink : mu::kIconNone;
        mu::DrawButton(di->hDC, di->rcItem, txt, hover, pressed, accent, false, d->font, icon);
        return TRUE;
    }
    case WM_MOUSEMOVE:
        mu::MuTrackHover(h, lp);
        return 0;
    case WM_MOUSELEAVE:
        mu::MuClearHover(h);
        return 0;
    case WM_CLOSE:
        d->done = true; d->ok = false;
        DestroyWindow(h);
        return 0;
    default:
        return DefWindowProcW(h, msg, wp, lp);
    }
}

// returns true when the user clicks "Connect" and fills ip/group; cancel/close returns false
static bool promptConnection(wchar_t* ipOut, wchar_t* groupOut) {
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    WNDCLASSW wc = {};
    wc.lpfnWndProc = connWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"ClientConnDlg";
    RegisterClassW(&wc);

    ConnDialog d;
    d.font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH, L"Segoe UI");
    d.bgBrush = CreateSolidBrush(mu::kBgDark);
    d.panelBrush = CreateSolidBrush(mu::kInput);

    // load the historical connection record as defaults
    wchar_t savedIP[64] = {}, savedGroup[64] = {};
    loadConfig(savedIP, savedGroup, 64);

    d.hwnd = CreateWindowExW(0, L"ClientConnDlg", L"screen_cast 学生端", WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                             CW_USEDEFAULT, CW_USEDEFAULT, 360, 220, nullptr, nullptr, hInst, &d);
    if (!d.hwnd) { DeleteObject(d.font); DeleteObject(d.bgBrush); DeleteObject(d.panelBrush); return false; }
    mu::ApplyDarkFrame(d.hwnd, true);

    auto mkEdit = [&](const wchar_t* txt, int x, int y, int w, int h, int id) -> HWND {
        HWND hc = CreateWindowExW(0, L"EDIT", txt, WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL | WS_TABSTOP,
                                  x, y, w, h, d.hwnd, (HMENU)(INT_PTR)id, hInst, nullptr);
        SendMessageW(hc, WM_SETFONT, (WPARAM)d.font, TRUE);
        mu::MuStyleEdit(hc);   // dark rounded border instead of the system light one
        return hc;
    };
    auto mkLabel = [&](const wchar_t* txt, int x, int y, int w, int h) {
        HWND lbl = CreateWindowExW(0, L"STATIC", txt, WS_CHILD | WS_VISIBLE, x, y, w, h, d.hwnd, nullptr, hInst, nullptr);
        SendMessageW(lbl, WM_SETFONT, (WPARAM)d.font, TRUE);
        return lbl;
    };
    // subtitle only (the app title now lives in the window title bar)
    mkLabel(L"输入教师端 IP 开始观看", 24, 18, 320, 16);

    mkLabel(L"教师端 IP:", 24, 44, 72, 20);
    d.ipEdit = mkEdit(savedIP, 104, 42, 220, 28, 101);
    mkLabel(L"组播组:", 24, 78, 60, 20);
    d.groupEdit = mkEdit(savedGroup, 104, 76, 220, 28, 102);
    HWND hbOk = CreateWindowExW(0, L"BUTTON", L"连接", WS_CHILD | WS_VISIBLE | BS_DEFPUSHBUTTON | BS_OWNERDRAW | BS_NOTIFY | WS_TABSTOP,
                                96, 118, 90, 32, d.hwnd, (HMENU)1, hInst, nullptr);
    SendMessageW(hbOk, WM_SETFONT, (WPARAM)d.font, TRUE);
    HWND hbCn = CreateWindowExW(0, L"BUTTON", L"取消", WS_CHILD | WS_VISIBLE | BS_OWNERDRAW | BS_NOTIFY | WS_TABSTOP,
                                196, 118, 90, 32, d.hwnd, (HMENU)2, hInst, nullptr);
    SendMessageW(hbCn, WM_SETFONT, (WPARAM)d.font, TRUE);

    ShowWindow(d.hwnd, SW_SHOW);
    // modal message loop: poll with PeekMessage so the loop can exit promptly once the button callback sets done
    // (GetMessageW blocks waiting for a posted message; after a cross-process SendMessage is handled it may never return)
    MSG msg;
    while (!d.done) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (IsDialogMessageW(d.hwnd, &msg)) continue;   // Enter=Connect (Esc=Cancel, Tab cycles focus)
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!d.done) Sleep(10);
    }
    wcscpy_s(ipOut, 64, d.ip);
    wcscpy_s(groupOut, 64, d.group);
    DeleteObject(d.font);
    DeleteObject(d.bgBrush);
    DeleteObject(d.panelBrush);
    return d.ok;
}

int main(int argc, char* argv[]) {
    SetUnhandledExceptionFilter(clientCrashHandler);
    // DPI awareness: without it Windows applies DPI virtualization to the process, GetClientRect/swap-chain buffer return
    // logical sizes (before scaling), mismatched with D3D's physical render target -> whole picture/cursor out of proportion.
    // Must be called before any window is created.
    SetProcessDPIAware();

    // teacher IP / multicast group: command-line args take precedence; with no args pop up the "Connect teacher" dialog to enter IP (double-click launch can choose too)
    char teacherIP[64] = "", group[64] = "";
    bool multicast = false;
    if (argc >= 2) {
        strncpy_s(teacherIP, 64, argv[1], 63);
        if (argc >= 3) strncpy_s(group, 64, argv[2], 63);
        multicast = group[0] != '\0';
    } else {
        // no command-line args: pop up the connection dialog (defaults come from the last-saved client_config.txt)
        wchar_t wip[64] = {}, wgr[64] = {};
        if (!promptConnection(wip, wgr)) return 0;   // user cancelled/closed the dialog -> exit immediately
        WideCharToMultiByte(CP_UTF8, 0, wip, -1, teacherIP, 63, nullptr, nullptr);
        WideCharToMultiByte(CP_UTF8, 0, wgr, -1, group, 63, nullptr, nullptr);
        multicast = group[0] != '\0';   // empty multicast group in the dialog means unicast
        // save this connection's info so the next launch auto-fills the dialog
        saveConfig(wip, wgr);
    }
    // also save the connection info to disk when command-line args were given (command-line args can be reused in the dialog next time)
    if (argc >= 2) {
        wchar_t wip[64] = {}; MultiByteToWideChar(CP_UTF8, 0, teacherIP, -1, wip, 63);
        wchar_t wgr[64] = {}; MultiByteToWideChar(CP_UTF8, 0, group, -1, wgr, 63);
        saveConfig(wip, wgr);
    }

    D3D11Renderer renderer;
    if (!renderer.create(L"screen_cast - 客户端 v" SCREENCAST_VERSION_WS, 960, 600)) {
        MessageBoxW(nullptr, L"渲染器初始化失败(请确认显卡支持 D3D11)", L"screen_cast 客户端 v" SCREENCAST_VERSION_WS, MB_ICONERROR);
        return 1;
    }

    FrameQueue queue;
    EncodedQueue encQueue;
    FFmpegDecoder decoder;
    std::atomic<uint64_t> decodedOk{ 0 };
    std::atomic<uint32_t> latestLatencyMs{ 0 };
    std::atomic<int> latestDarkPct{ -1 };     // dark-pixel percent of the latest decoded frame %(-1=unknown), used for the dark-screen hint
    std::atomic<bool> decodePrimed{ false };  // first frame decoded (receive thread stops keyframe requests based on this)

    uint64_t lastStats = 0, lastDecoded = 0, lastNoSigMs = 0;
    bool wasDisconnected = false;   // auto-reconnect edge detection (rising edge = trigger a reset + keyframe re-request)
    auto t0 = std::chrono::steady_clock::now();

    // receive thread: recv -> reassemble(FEC) -> copy the encoded frame into the queue (no decode, network not dragged by CPU)
    ClientReceiver rx;
    if (!rx.start(teacherIP, group, 5004, 5005, [&](const ReceivedFrame& f) {
        try {
            if (!f.data || f.data->empty() || f.data->size() > 8 * 1024 * 1024) return;
            EncodedQueue::Item it;
            it.data = *f.data;          // copy: the reassembly buffer is only valid inside this callback
            it.captureMs = f.captureMs;
            encQueue.push(std::move(it));
        } catch (...) {}
    })) {
        wchar_t tip[192];
        swprintf_s(tip, L"收包线程启动失败(检查教师端 IP 与网络): %hs", teacherIP);
        MessageBoxW(nullptr, tip, L"screen_cast 客户端", MB_ICONERROR);
        return 1;
    }
    rx.setDecodePrimed(&decodePrimed);

    // dedicated decode thread: pop encoded frame -> H.264 decode -> stats/latency/enqueue BGRA
    std::thread decodeThread([&] {
        EncodedQueue::Item it;
        while (g_running.load()) {
            try {
                if (!encQueue.pop(it)) { std::this_thread::sleep_for(std::chrono::milliseconds(1)); continue; }
                if (decoder.decode(it.data.data(), (int)it.data.size(), it.captureMs)) {
                    // read the decoded output under the decoder's lock; if the reconnect edge closed the
                    // decoder right after decode() returned, drop this stale frame instead of enqueueing
                    // a null/empty frame (avoids pushing garbage that would only be discarded downstream)
                    const uint8_t* d = decoder.bgra();
                    int w = decoder.width(), h = decoder.height();
                    uint64_t pms = decoder.ptsMs();
                    if (!d || w <= 0 || h <= 0) continue;
                    if (!decodePrimed.load()) decodePrimed.store(true);   // first frame decoded: stop keyframe requests
                    if (decodedOk.load() % 10 == 0) {
                        int dark = 0, cnt = 0;
                        for (int y = 0; y < h; y += 48)
                            for (int x = 0; x < w; x += 48) {
                                const uint8_t* q = d + ((size_t)y * w + (size_t)x) * 4;
                                if (q[0] + q[1] + q[2] < 120) dark++;
                                cnt++;
                            }
                        latestDarkPct.store(cnt ? (dark * 100) / cnt : -1);
                    }
                    queue.push(d, w, h, pms);
                    ++decodedOk;
                    uint32_t now = (uint32_t)GetTickCount64();
                    if (it.captureMs && now > (uint32_t)it.captureMs) {
                        uint32_t dl = now - (uint32_t)it.captureMs;
                        latestLatencyMs.store(dl <= 500 ? dl : kLatUnavailable);
                    }
                }
            } catch (...) {}
        }
    });

    uint64_t lastPresentMs = 0;   // last real-present time (steady clock, 0 = show the first frame immediately)
    // last-frame cache: differential skip-frames + 2s keep-alive make video frames sparse (~2s/frame); if we only
    // present() on new frames, the teacher's pointer overlay would freeze at the old "last presented" position.
    // When the queue is empty, redraw with the cached frame at ~20Hz (re-upload texture + redraw cursor), so the
    // pointer keeps following the teacher's desktop moves in real time.
    std::vector<uint8_t> lastBgra;
    int lastFW = 0, lastFH = 0;
    uint64_t lastRepaintMs = 0;
    while (g_running && renderer.pumpMessages()) {
        std::vector<uint8_t> frame;
        int fw = 0, fh = 0;
        uint64_t ms = 0, frontMs = 0;
        if (queue.peekMs(&frontMs)) {
            // front has a frame: consume paced at ~30fps (33ms), smoothing out network burst-jitter
            uint64_t nowMs = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            if (lastPresentMs == 0 || nowMs >= lastPresentMs + 33) {
                if (queue.pop(frame, fw, fh, ms)) {
                    // sync the teacher's pointer state (updated by the receive thread, consumed on the main thread here)
                    float cxn = 0.0f, cyn = 0.0f; bool cv = false;
                    if (rx.cursorState(cxn, cyn, cv)) renderer.setCursor(cxn, cyn, cv);
                    renderer.setNoSignal(false);   // real decoded picture
                    renderer.present(frame.data(), fw, fh);
                    lastBgra = std::move(frame);   // cache the last frame (move, zero extra copy)
                    lastFW = fw; lastFH = fh;
                    lastPresentMs = nowMs;   // baseline on this frame's present time, one frame per 33ms
                }
            } else {
                Sleep(1);   // not yet at the pace point, brief wait (keeps the window responsive)
            }
        } else {
            // queue empty: show the next frame immediately on arrival (catch-up), keeping low latency
            lastPresentMs = 0;
            // static-desktop refresh: video frames are sparse but the cursor moves, so redraw the cached frame at
            // ~20Hz (with cursor re-sync), keeping the pointer overlay following the teacher's mouse in real time
            // -- fixes "client can't see the server mouse on a static desktop"
            uint64_t nowMs = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                std::chrono::steady_clock::now() - t0).count();
            if (!lastBgra.empty() && nowMs - lastRepaintMs >= 50) {
                lastRepaintMs = nowMs;
                float cxn = 0.0f, cyn = 0.0f; bool cv = false;
                if (rx.cursorState(cxn, cyn, cv)) {
                    renderer.setCursor(cxn, cyn, cv);
                    renderer.setNoSignal(false);   // real cached picture, not the waiting placeholder
                    renderer.present(lastBgra.data(), lastFW, lastFH);
                }
            }
            if (!decodePrimed.load()) {
                uint64_t el = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
                    std::chrono::steady_clock::now() - t0).count();
                // waiting-for-teacher placeholder: a CALM, static dark-blue screen (no harsh blue/black
                // blinking -- the old alternating pattern read as "broken screen"). Repaint at ~2.5Hz so
                // window resizes still get a fresh buffer; the title bar carries the real status text.
                if (el > 2000 && el - lastNoSigMs > 400) {
                    lastNoSigMs = el;
                    static std::vector<uint8_t> nosig;
                    const int nw = 1280, nh = 720;
                    if (nosig.size() != (size_t)nw * nh * 4) nosig.assign((size_t)nw * nh * 4, 0);
                    // BGRA: static dark slate blue, clearly "waiting state" but never flickering
                    for (size_t i = 0; i < nosig.size(); i += 4) {
                        nosig[i + 0] = 76; nosig[i + 1] = 34; nosig[i + 2] = 26; nosig[i + 3] = 255;
                    }
                    renderer.setNoSignal(true);   // not a real picture: no drag-hand cursor over it
                    renderer.present(nosig.data(), nw, nh);
                }
            }
            std::this_thread::sleep_for(std::chrono::milliseconds(2));
        }

        uint64_t now = (uint64_t)std::chrono::duration_cast<std::chrono::milliseconds>(
            std::chrono::steady_clock::now() - t0).count();
        if (now - lastStats > 1000) {
            uint64_t dt = now - lastStats;
            uint64_t decoded = decodedOk.load();
            double fps = dt ? (double)(decoded - lastDecoded) * 1000.0 / (double)dt : 0.0;
            lastStats = now; lastDecoded = decoded;
            uint64_t lost = rx.lostFrames(), got = rx.frames();
            double lossPct = (lost + got) ? (double)lost * 100.0 / (double)(lost + got) : 0.0;
            int q = queue.sizeSafe();
            uint32_t lat = latestLatencyMs.load();
            wchar_t title[256];
            const wchar_t* mode = multicast ? L"(组播)" : L"(单播)";
            int darkPct = latestDarkPct.load();
            bool dark = got > 0 && darkPct >= 0 && darkPct > 60;
            // stream-loss detection: received picture before but no new frame for >5s (teacher exited/crashed/network down).
            // While the teacher is alive, differential keep-alive sends at least one frame every 2s, so the 5s threshold
            // won't false-positive (nor on a static desktop).
            uint64_t rxLastMs = rx.lastFrameMs();
            bool disconnected = got > 0 && rxLastMs != 0 &&
                                (uint64_t)GetTickCount64() - rxLastMs > 5000;
            if (disconnected && !wasDisconnected) {
                // ---- auto-reconnect (rising edge) ----
                // the teacher is gone: reset the decode-prime flag so the receive thread requests a keyframe
                // every 300ms again, and clear the frame-sequence tracking so the teacher's restarted frameId
                // numbering doesn't fake a loss gap. Also reset the decoder: a restarted teacher may use a
                // different resolution/SPS, the old context's extradata would be stale (ffmpeg internal
                // re-init can usually recover, but a clean reset guarantees it). When the teacher comes back
                // the first keyframe lands and the picture re-enters within a second, no manual restart needed.
                wasDisconnected = true;
                decodePrimed.store(false);
                rx.requestReset();
                decoder.close();
            } else if (!disconnected && wasDisconnected) {
                wasDisconnected = false;   // recovered: stream is flowing again
            }
            if (got == 0) {
                latestDarkPct.store(-1);
                swprintf_s(title, L"screen_cast 客户端 v" SCREENCAST_VERSION_WS L" | 教师端 %hs | 未收到画面 - 等待教师推流 | 缩放 %.0f%%", teacherIP, (double)(renderer.effectiveZoom() * 100.0));
                renderer.setTitle(title);
            } else if (disconnected)
                swprintf_s(title, L"screen_cast 客户端 v" SCREENCAST_VERSION_WS L"%s | 教师端 %hs | 连接中断 - 自动重连中... | 最后 %.0f FPS | 缩放 %.0f%%",
                    mode, teacherIP, fps, (double)(renderer.effectiveZoom() * 100.0));
            else if (lat == kLatUnavailable)
                swprintf_s(title, L"screen_cast 客户端 v" SCREENCAST_VERSION_WS L"%s | 教师端 %hs | %.0f FPS | 丢包 %.1f%% | 延迟 -- ms | 队列 %d | 缩放 %.0f%%",
                    mode, teacherIP, fps, lossPct, q, (double)(renderer.effectiveZoom() * 100.0));
            else
                swprintf_s(title, L"screen_cast 客户端 v" SCREENCAST_VERSION_WS L"%s | 教师端 %hs | %.0f FPS | 丢包 %.1f%% | 延迟 %u ms | 队列 %d | 缩放 %.0f%%%s",
                    mode, teacherIP, fps, lossPct, lat, q, (double)(renderer.effectiveZoom() * 100.0),
                    dark ? L" | 画面偏暗(桌面深色? 放个亮色窗口验证)" : L"");
            renderer.setTitle(title);
        }
    }
    g_running = false;
    rx.stop();
    if (decodeThread.joinable()) decodeThread.join();
    return 0;
}
