// installer.cpp -- screen_cast screen-sharing installer (pure Win32, no external deps)
// usage: installer.exe                pop up the install UI (single-select student/teacher + install dir)
//       installer.exe /uninstall     uninstall based on registry info
// payload files must be in the same directory as installer.exe (source files taken from the exe's own dir):
//   teacher_gui.exe / client_gui.exe + FFmpeg runtime DLLs (avcodec/avutil/swscale/swresample)
#include <winsock2.h>
#include <windows.h>
#include <shlobj.h>
#include <objbase.h>
#include <shobjidl.h>
#include <shellapi.h>
#include <cstdio>
#include <cwchar>
#include <string>
#include <vector>
#include <filesystem>
#include <system_error>
#include "ui/modern_ui.h"
#include "version.h"
#pragma comment(lib, "shell32.lib")
#pragma comment(lib, "ole32.lib")
#pragma comment(lib, "user32.lib")
#pragma comment(lib, "gdi32.lib")
// GUI subsystem (no black console window), the entry stays main(): explicitly set the CRT entry
#pragma comment(linker, "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup")

namespace fs = std::filesystem;

static const wchar_t* kAppName   = L"screen_cast 屏幕共享";
static const wchar_t* kVersion   = SCREENCAST_VERSION_WS;   // 与 version.h 同步
static const wchar_t* kUninstallKey = L"Software\\Microsoft\\Windows\\CurrentVersion\\Uninstall\\ScreenCast";

// ---------- utility functions ----------

// this exe's directory (payload source files are taken from here at install time)
static std::wstring exeDir() {
    wchar_t buf[MAX_PATH]; GetModuleFileNameW(nullptr, buf, MAX_PATH);
    std::wstring p = buf;
    auto pos = p.find_last_of(L'\\');
    return pos == std::wstring::npos ? L"" : p.substr(0, pos);
}

// default install dir: %LOCALAPPDATA%\Programs\ScreenCast (no admin rights needed, VS Code-style)
static std::wstring defaultInstallDir() {
    wchar_t buf[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_LOCAL_APPDATA, nullptr, SHGFP_TYPE_CURRENT, buf))) {
        return std::wstring(buf) + L"\\Programs\\ScreenCast";
    }
    return L"C:\\ScreenCast";
}

static std::wstring startMenuDir() {
    wchar_t buf[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_STARTMENU, nullptr, SHGFP_TYPE_CURRENT, buf))) {
        return std::wstring(buf) + L"\\Programs\\ScreenCast";
    }
    return L"";
}

static std::wstring desktopDir() {
    wchar_t buf[MAX_PATH] = {};
    if (SUCCEEDED(SHGetFolderPathW(nullptr, CSIDL_DESKTOPDIRECTORY, nullptr, SHGFP_TYPE_CURRENT, buf))) {
        return std::wstring(buf);
    }
    return L"";
}

// create a .lnk shortcut
static bool createShortcut(const std::wstring& lnkPath, const std::wstring& target) {
    IShellLinkW* sl = nullptr;
    if (FAILED(CoCreateInstance(CLSID_ShellLink, nullptr, CLSCTX_INPROC_SERVER, IID_PPV_ARGS(&sl)))) return false;
    sl->SetPath(target.c_str());
    auto pos = target.find_last_of(L'\\');
    if (pos != std::wstring::npos) {
        std::wstring dir = target.substr(0, pos);
        sl->SetWorkingDirectory(dir.c_str());
    }
    sl->SetIconLocation(target.c_str(), 0);
    IPersistFile* pf = nullptr;
    HRESULT hr = sl->QueryInterface(IID_PPV_ARGS(&pf));
    if (SUCCEEDED(hr)) {
        hr = pf->Save(lnkPath.c_str(), TRUE);
        pf->Release();
    }
    sl->Release();
    return SUCCEEDED(hr);
}

// write the uninstall registry entry (HKCU, shown in "Settings -> Apps")
static void writeUninstallReg(const std::wstring& dir, const std::wstring& exeName) {
    HKEY hk = nullptr;
    if (RegCreateKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0, nullptr, 0, KEY_WRITE, nullptr, &hk, nullptr) != ERROR_SUCCESS)
        return;
    std::wstring exePath = dir + L"\\" + exeName;
    std::wstring uninst = L"\"" + dir + L"\\installer.exe\" /uninstall";
    auto setStr = [&](const wchar_t* name, const std::wstring& v) {
        RegSetValueExW(hk, name, 0, REG_SZ, (const BYTE*)v.c_str(), (DWORD)((v.size() + 1) * sizeof(wchar_t)));
    };
    setStr(L"DisplayName", kAppName);
    setStr(L"DisplayVersion", kVersion);
    setStr(L"Publisher", L"screen_cast");
    setStr(L"InstallLocation", dir);
    setStr(L"DisplayIcon", exePath);
    setStr(L"UninstallString", uninst);
    DWORD one = 1;
    RegSetValueExW(hk, L"NoModify", 0, REG_DWORD, (const BYTE*)&one, sizeof(one));
    RegSetValueExW(hk, L"NoRepair", 0, REG_DWORD, (const BYTE*)&one, sizeof(one));
    RegCloseKey(hk);
}

static std::wstring regGetString(const wchar_t* name) {
    HKEY hk = nullptr;
    if (RegOpenKeyExW(HKEY_CURRENT_USER, kUninstallKey, 0, KEY_READ, &hk) != ERROR_SUCCESS) return L"";
    wchar_t buf[MAX_PATH] = {};
    DWORD sz = sizeof(buf), type = 0;
    LONG r = RegQueryValueExW(hk, name, nullptr, &type, (BYTE*)buf, &sz);
    RegCloseKey(hk);
    return (r == ERROR_SUCCESS && type == REG_SZ) ? buf : L"";
}

static void deleteRegKey() {
    RegDeleteKeyW(HKEY_CURRENT_USER, kUninstallKey);
}

// delete all files in the install dir (keep the running installer.exe itself), then a delayed script clears the dir
static void deleteInstallDirContents(const std::wstring& dir) {
    std::error_code ec;
    if (!fs::exists(dir, ec)) return;
    for (const auto& e : fs::directory_iterator(dir, ec)) {
        std::wstring name = e.path().filename().wstring();
        if (name == L"installer.exe") continue;   // the running exe can't be deleted; leave it to the delayed script
        fs::remove_all(e.path(), ec);
    }
}

// uninstall
static int runUninstall(bool noConfirm) {
    std::wstring dir = regGetString(L"InstallLocation");
    if (dir.empty()) {
        MessageBoxW(nullptr, L"未找到安装记录(可能已卸载)。", kAppName, MB_ICONINFORMATION | MB_OK);
        return 0;
    }
    if (!noConfirm &&
        MessageBoxW(nullptr, (L"确定要卸载 ScreenCast 屏幕共享吗?\n\n目录: " + dir + L"\n").c_str(),
                    kAppName, MB_ICONQUESTION | MB_YESNO) != IDYES)
        return 0;
    // 1) delete shortcuts
    std::wstring sm = startMenuDir();
    if (!sm.empty()) { std::error_code ec; fs::remove_all(sm, ec); }
    std::wstring desk = desktopDir();
    if (!desk.empty()) {
        std::error_code ec;
        fs::remove(desk + L"\\学生端 - screen_cast.lnk", ec);
        fs::remove(desk + L"\\教师端 - screen_cast.lnk", ec);
    }
    // 2) delete the registry
    deleteRegKey();
    // 3) delete the dir contents (keep self), generate a delayed-cleanup script to remove the rest
    deleteInstallDirContents(dir);
    // TEMP may be unset (constructing a wstring from a null _wgetenv is UB), so fall back to GetTempPathW
    std::wstring tmp;
    if (const wchar_t* te = _wgetenv(L"TEMP"); te && te[0]) tmp = te;
    else {
        wchar_t tb[MAX_PATH] = {};
        DWORD n = GetTempPathW(MAX_PATH, tb);
        if (n > 0 && n < MAX_PATH) tmp = tb;
    }
    if (tmp.empty()) tmp = L".";   // last resort: current dir (installer runs from the install dir)
    std::wstring bat = tmp + L"\\screen_cast_uninstall.bat";
    {
        FILE* f = _wfopen(bat.c_str(), L"w");
        if (f) {
            fwprintf(f, L"@echo off\r\ntimeout /t 1 /nobreak >nul\r\n");
            fwprintf(f, L"rmdir /s /q \"%ls\" >nul 2>&1\r\n", dir.c_str());
            fwprintf(f, L"del \"%%~f0\"\r\n");
            fclose(f);
        }
    }
    std::wstring cmd = L"cmd /c start \"\" /b \"" + bat + L"\"";
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(nullptr, &cmd[0], nullptr, nullptr, FALSE, CREATE_NO_WINDOW, nullptr, nullptr, &si, &pi)) {
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
    }
    MessageBoxW(nullptr, L"卸载完成。", kAppName, MB_ICONINFORMATION | MB_OK);
    return 0;
}

// ---------- install UI ----------

struct InstallDlg {
    HWND hwnd = nullptr, editDir = nullptr;
    int   choice = 0;       // 0=student 1=teacher
    bool  done = false, ok = false;
    wchar_t dir[MAX_PATH] = L"";
    HFONT font = nullptr;
    HBRUSH bgBrush = nullptr, panelBrush = nullptr;
};

// perform the install: copy files + shortcuts + uninstall registry. returns true on success, fills errOut on failure.
static bool performInstall(const std::wstring& destDirIn, int choice, std::wstring& errOut) {
    std::wstring destDir = destDirIn;
    while (!destDir.empty() && destDir.back() == L'\\') destDir.pop_back();
    if (destDir.empty()) { errOut = L"未指定安装目录。"; return false; }
    const wchar_t* exeName = choice == 0 ? L"client_gui.exe" : L"teacher_gui.exe";
    const wchar_t* linkName = choice == 0 ? L"学生端 - screen_cast.lnk" : L"教师端 - screen_cast.lnk";

    std::wstring payloadDir = exeDir();
    std::error_code ec;
    if (!fs::exists(payloadDir + L"\\" + exeName, ec)) {
        errOut = L"未找到组件文件:\n" + payloadDir + L"\\" + exeName + L"\n请将安装程序与程序文件放在同一目录。";
        return false;
    }
    fs::create_directories(destDir, ec);

    // copy the selected end's exe
    fs::copy_file(payloadDir + L"\\" + exeName, destDir + L"\\" + exeName,
                  fs::copy_options::overwrite_existing, ec);
    // copy the FFmpeg runtime DLLs (all avcodec/avutil/swscale/swresample)
    for (const auto& e : fs::directory_iterator(payloadDir, ec)) {
        std::wstring n = e.path().filename().wstring();
        if (n.size() < 4 || n.compare(n.size() - 4, 4, L".dll") != 0) continue;
        if (n.rfind(L"avcodec-", 0) == 0 || n.rfind(L"avutil-", 0) == 0 ||
            n.rfind(L"swscale-", 0) == 0 || n.rfind(L"swresample-", 0) == 0) {
            fs::copy_file(e.path(), destDir + L"\\" + n, fs::copy_options::overwrite_existing, ec);
        }
    }
    // also copy installer.exe into the install dir (uninstall runs from there)
    std::wstring self = payloadDir + L"\\installer.exe";
    if (fs::exists(self, ec) && fs::absolute(payloadDir, ec) != fs::absolute(destDir, ec))
        fs::copy_file(self, destDir + L"\\installer.exe", fs::copy_options::overwrite_existing, ec);

    // shortcuts
    std::wstring sm = startMenuDir();
    if (!sm.empty()) {
        std::error_code e2;
        fs::create_directories(sm, e2);
        createShortcut(sm + L"\\" + linkName, destDir + L"\\" + exeName);
    }
    std::wstring desk = desktopDir();
    if (!desk.empty())
        createShortcut(desk + L"\\" + linkName, destDir + L"\\" + exeName);

    writeUninstallReg(destDir, exeName);
    return true;
}

static void doInstall(InstallDlg* d) {
    std::wstring destDir = d->dir;
    std::wstring err;
    HCURSOR hWait = LoadCursor(nullptr, IDC_WAIT);
    HCURSOR hOld = SetCursor(hWait);
    bool ok = performInstall(destDir, d->choice, err);
    SetCursor(hOld);
    if (!ok) {
        MessageBoxW(d->hwnd, err.c_str(), kAppName, MB_ICONERROR);
        return;
    }
    const wchar_t* display = d->choice == 0 ? L"学生端" : L"教师端";
    std::wstring msg = L"「" + std::wstring(display) + L"」安装完成!\n\n位置: " + destDir +
                       L"\n已创建开始菜单和桌面快捷方式。\n\n可在「设置 -> 应用」中卸载。";
    MessageBoxW(d->hwnd, msg.c_str(), kAppName, MB_ICONINFORMATION | MB_OK);
}

static LRESULT CALLBACK instWndProc(HWND h, UINT msg, WPARAM wp, LPARAM lp) {
    InstallDlg* d = (InstallDlg*)GetWindowLongPtrW(h, GWLP_USERDATA);
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
    case WM_ERASEBKGND: {
        // subtle vertical gradient: lighter at the top (elevated look), deep at the bottom
        RECT rc; GetClientRect(h, &rc);
        mu::GradientFillV((HDC)wp, rc, RGB(0x28, 0x2D, 0x47), mu::kBgDark, 20);
        return 1;
    }
    case WM_CTLCOLORSTATIC:
    case WM_CTLCOLORBTN: {
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
        wchar_t txt[128]; GetWindowTextW(di->hwndItem, txt, 128);
        if ((int)di->CtlID == 1) {   // 安装 = accent + download icon
            mu::DrawButton(di->hDC, di->rcItem, txt, hover, pressed, true, false, d->font, mu::kIconDown);
        } else if ((int)di->CtlID == 202) {   // 浏览 = ghost
            mu::DrawButton(di->hDC, di->rcItem, txt, hover, pressed, false, false, d->font);
        } else if ((int)di->CtlID == 101 || (int)di->CtlID == 102) {   // 单选框 -> 自绘圆形 radio
            bool checked = SendMessageW(di->hwndItem, BM_GETCHECK, 0, 0) == BST_CHECKED;
            mu::DrawRadio(di->hDC, di->rcItem, txt, checked, hover, false, d->font);
        } else {   // 取消 = ghost
            mu::DrawButton(di->hDC, di->rcItem, txt, hover, pressed, false, false, d->font);
        }
        return TRUE;
    }
    case WM_MOUSEMOVE:
        mu::MuTrackHover(h, lp);
        return 0;
    case WM_MOUSELEAVE:
        mu::MuClearHover(h);
        return 0;
    case WM_COMMAND: {
        int id = LOWORD(wp);
        switch (id) {
        case 101: d->choice = 0; return 0;
        case 102: d->choice = 1; return 0;
        case 202: {   // browse directory
            IFileDialog* fdlg = nullptr;
            if (SUCCEEDED(CoCreateInstance(CLSID_FileOpenDialog, nullptr, CLSCTX_INPROC_SERVER,
                                           IID_PPV_ARGS(&fdlg)))) {
                fdlg->SetOptions(FOS_PICKFOLDERS | FOS_FORCEFILESYSTEM);
                fdlg->SetTitle(L"选择安装目录");
                if (d->dir[0]) {
                    IShellItem* si = nullptr;
                    if (SUCCEEDED(SHCreateItemFromParsingName(d->dir, nullptr, IID_PPV_ARGS(&si)))) {
                        fdlg->SetFolder(si); si->Release();
                    }
                }
                if (SUCCEEDED(fdlg->Show(h))) {
                    IShellItem* item = nullptr;
                    if (SUCCEEDED(fdlg->GetResult(&item)) && item) {
                        PWSTR path = nullptr;
                        if (SUCCEEDED(item->GetDisplayName(SIGDN_FILESYSPATH, &path)) && path) {
                            wcscpy_s(d->dir, path);
                            SetWindowTextW(d->editDir, d->dir);
                            CoTaskMemFree(path);
                        }
                        item->Release();
                    }
                }
                fdlg->Release();
            }
            return 0;
        }
        case 1:   // install
            if (d->dir[0] == L'\0') {
                wchar_t buf[MAX_PATH];
                GetWindowTextW(d->editDir, buf, MAX_PATH);
                wcscpy_s(d->dir, buf);
            }
            doInstall(d);
            return 0;
        case 2:   // cancel
            d->done = true; d->ok = false;
            DestroyWindow(h);
            return 0;
        }
        return 0;
    }
    case WM_CLOSE:
        d->done = true; d->ok = false;
        DestroyWindow(h);
        return 0;
    default:
        return DefWindowProcW(h, msg, wp, lp);
    }
}

static int runInstallerUI() {
    HINSTANCE hInst = GetModuleHandleW(nullptr);
    WNDCLASSW wc = {};
    wc.lpfnWndProc = instWndProc;
    wc.hInstance = hInst;
    wc.hCursor = LoadCursor(nullptr, IDC_ARROW);
    wc.hbrBackground = (HBRUSH)(COLOR_BTNFACE + 1);
    wc.lpszClassName = L"ScreenCastInstDlg";
    RegisterClassW(&wc);

    InstallDlg d;
    d.font = CreateFontW(-15, 0, 0, 0, FW_NORMAL, 0, 0, 0, DEFAULT_CHARSET,
                         OUT_DEFAULT_PRECIS, CLIP_DEFAULT_PRECIS, CLEARTYPE_QUALITY,
                         DEFAULT_PITCH, L"Segoe UI");
    d.bgBrush = CreateSolidBrush(mu::kBgDark);
    d.panelBrush = CreateSolidBrush(mu::kInput);

    wcscpy_s(d.dir, defaultInstallDir().c_str());
    d.hwnd = CreateWindowExW(0, L"ScreenCastInstDlg", (std::wstring(kAppName) + L" 安装").c_str(),
                             WS_OVERLAPPED | WS_CAPTION | WS_SYSMENU,
                             CW_USEDEFAULT, CW_USEDEFAULT, 520, 330, nullptr, nullptr, hInst, &d);
    if (!d.hwnd) { DeleteObject(d.font); DeleteObject(d.bgBrush); DeleteObject(d.panelBrush); return 1; }
    mu::ApplyDarkFrame(d.hwnd, true);

    auto mkStatic = [&](const wchar_t* txt, int x, int y, int w, int h) {
        HWND c = CreateWindowExW(0, L"STATIC", txt, WS_CHILD | WS_VISIBLE, x, y, w, h, d.hwnd, nullptr, hInst, nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)d.font, TRUE);
        return c;
    };
    auto mkRadio = [&](const wchar_t* txt, int x, int y, int w, int h, int id, bool first, bool checked) {
        DWORD st = WS_CHILD | WS_VISIBLE | BS_AUTORADIOBUTTON | BS_OWNERDRAW | BS_NOTIFY | WS_TABSTOP | (first ? WS_GROUP : 0);
        HWND c = CreateWindowExW(0, L"BUTTON", txt, st, x, y, w, h, d.hwnd, (HMENU)(INT_PTR)id, hInst, nullptr);
        if (checked) SendMessageW(c, BM_SETCHECK, BST_CHECKED, 0);
        SendMessageW(c, WM_SETFONT, (WPARAM)d.font, TRUE);
        return c;
    };
    auto mkButton = [&](const wchar_t* txt, int x, int y, int w, int h, int id, DWORD style) {
        HWND c = CreateWindowExW(0, L"BUTTON", txt, WS_CHILD | WS_VISIBLE | style | WS_TABSTOP,
                                 x, y, w, h, d.hwnd, (HMENU)(INT_PTR)id, hInst, nullptr);
        SendMessageW(c, WM_SETFONT, (WPARAM)d.font, TRUE);
        return c;
    };

    mkStatic(L"请选择要安装的组件(单选):", 26, 20, 300, 24);
    mkRadio(L"学生端  ——  观看教师端实时屏幕画面", 42, 52, 400, 28, 101, true, true);
    mkRadio(L"教师端  ——  把自己的屏幕画面分享给学生", 42, 84, 400, 28, 102, false, false);
    mkStatic(L"安装目录:", 26, 132, 72, 24);
    d.editDir = CreateWindowExW(0, L"EDIT", d.dir, WS_CHILD | WS_VISIBLE | WS_BORDER | ES_AUTOHSCROLL,
                                104, 130, 312, 28, d.hwnd, (HMENU)201, hInst, nullptr);
    SendMessageW(d.editDir, WM_SETFONT, (WPARAM)d.font, TRUE);
    mu::MuStyleEdit(d.editDir);   // dark rounded border instead of the system light one
    mkButton(L"浏览...", 428, 130, 66, 28, 202, BS_OWNERDRAW | BS_NOTIFY);
    mkStatic(L"将同时创建「开始菜单」和「桌面」快捷方式, 可在设置->应用 中卸载。", 26, 170, 470, 22);
    mkButton(L"安装", 312, 232, 90, 34, 1, BS_DEFPUSHBUTTON | BS_OWNERDRAW | BS_NOTIFY);
    mkButton(L"取消", 414, 232, 80, 34, 2, BS_OWNERDRAW | BS_NOTIFY);

    ShowWindow(d.hwnd, SW_SHOW);
    MSG msg;
    while (!d.done) {
        while (PeekMessageW(&msg, nullptr, 0, 0, PM_REMOVE)) {
            if (IsDialogMessageW(d.hwnd, &msg)) continue;   // Enter=install, Esc=cancel
            TranslateMessage(&msg);
            DispatchMessageW(&msg);
        }
        if (!d.done) Sleep(10);
    }
    DeleteObject(d.font);
    DeleteObject(d.bgBrush);
    DeleteObject(d.panelBrush);
    return d.ok ? 0 : 1;
}

int main(int argc, char* argv[]) {
    CoInitializeEx(nullptr, COINIT_APARTMENTTHREADED);
    SetProcessDPIAware();
    // /uninstall [/y]: command-line uninstall (/y skips the confirm, for automation/silent scenarios)
    bool sawUninstall = false, sawY = false;
    for (int i = 1; i < argc; ++i) {
        if (_stricmp(argv[i], "/uninstall") == 0 || _stricmp(argv[i], "-uninstall") == 0) sawUninstall = true;
        if (_stricmp(argv[i], "/y") == 0 || _stricmp(argv[i], "-y") == 0) sawY = true;
    }
    if (sawUninstall) {
        int r = runUninstall(sawY); CoUninitialize(); return r;
    }
    // /silent [dir] [student|teacher]: silent install (for automation/verification, no UI)
    for (int i = 1; i < argc; ++i) {
        if (_stricmp(argv[i], "/silent") == 0 || _stricmp(argv[i], "-silent") == 0) {
            std::wstring dir = defaultInstallDir();
            int choice = 0;
            for (int j = i + 1; j < argc; ++j) {
                if (_stricmp(argv[j], "teacher") == 0) choice = 1;
                else if (_stricmp(argv[j], "student") == 0) choice = 0;
                else {
                    std::wstring a = std::wstring(argv[j], argv[j] + strlen(argv[j]));
                    if (!a.empty() && (a.find(L'\\') != std::wstring::npos || a.find(L':') != std::wstring::npos))
                        dir = a;
                }
            }
            std::wstring err;
            bool ok = performInstall(dir, choice, err);
            CoUninitialize();
            return ok ? 0 : 1;
        }
    }
    int r = runInstallerUI();
    CoUninitialize();
    return r;
}