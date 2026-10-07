#pragma once
// crash_report.h -- unified crash logging NEXT TO THE EXECUTABLE, for both teacher and client.
// Covers all three crash kinds:
//   1) SEH access violations (null deref / out-of-bounds)  -> minidump (<app>_crash.dmp) + log line
//   2) std::terminate (uncaught C++ exception / bad_alloc / bad_function_call / double-join)
//      -> log line only (terminate aborts without going through SEH, so no dump is possible)
//   3) abort() / signal(SIGABRT) (FFmpeg internal asserts) -> log line
// All entries append to <app>_crash.log so a long-running session's crashes accumulate.
//
// WHY NOT THE CWD: the old version wrote "<app>_crash.log" as a RELATIVE path, i.e. into the current
// working directory. A student machine launches the client from a shortcut (CWD = the shortcut's
// "start in", often C:\Windows\System32) or from a deployment script, and those directories are not
// writable by a normal user -- so every crash was lost with no trace at all ("it just dies sometimes").
// The log/dump now go to the directory holding the .exe (always writable for the installing user),
// with %TEMP% as a fallback.
#include <windows.h>
#include <dbghelp.h>
#include <csignal>
#include <cstdio>
#include <cstring>
#include <string>
#include <exception>

#pragma comment(lib, "dbghelp.lib")

namespace crash_report {

static const char* g_appName = "app";
static std::string g_dir;                       // resolved once, "<dir>\\" with a trailing slash
static char        g_stage[128] = "startup";    // last milestone the app reported (see setStage)

static void resolveDir() {
    if (!g_dir.empty()) return;
    char exe[MAX_PATH] = { 0 };
    DWORD n = GetModuleFileNameA(nullptr, exe, MAX_PATH);
    if (n > 0 && n < MAX_PATH) {
        const char* slash = strrchr(exe, '\\');
        if (slash) g_dir.assign(exe, (size_t)(slash - exe) + 1);
    }
    if (g_dir.empty()) {   // fallback: %TEMP%
        char tmp[MAX_PATH] = { 0 };
        DWORD m = GetTempPathA(MAX_PATH, tmp);
        if (m > 0 && m < MAX_PATH) g_dir.assign(tmp, m);
        if (g_dir.empty() || g_dir[g_dir.size() - 1] != '\\') g_dir += '\\';
    }
}

// record what the app was doing, so a crash log says more than "it died". Cheap: a memcpy.
static void setStage(const char* s) {
    if (!s) return;
    snprintf(g_stage, sizeof(g_stage), "%s", s);
}

static void writeLog(const char* kind, const char* msg) {
    resolveDir();
    SYSTEMTIME st;
    GetLocalTime(&st);
    std::string path = g_dir + g_appName + "_crash.log";
    FILE* f = fopen(path.c_str(), "a");   // append: every crash in one file
    if (!f) return;
    fprintf(f, "[%04d-%02d-%02d %02d:%02d:%02d] [%s] stage=%s %s\n",
            st.wYear, st.wMonth, st.wDay, st.wHour, st.wMinute, st.wSecond, kind, g_stage, msg);
    fclose(f);
}

static LONG WINAPI sehHandler(EXCEPTION_POINTERS* ep) {
    char msg[256];
    snprintf(msg, sizeof(msg), "code=0x%08X addr=%p",
             ep->ExceptionRecord->ExceptionCode, ep->ExceptionRecord->ExceptionAddress);
    writeLog("SEH", msg);

    resolveDir();
    std::string dmp = g_dir + g_appName + "_crash.dmp";
    HANDLE hFile = CreateFileA(dmp.c_str(), GENERIC_WRITE, 0, nullptr,
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
    return EXCEPTION_EXECUTE_HANDLER;
}

static void terminateHandler() {
    char buf[512];
    snprintf(buf, sizeof(buf), "std::terminate (no exception detail)");
    if (std::exception_ptr ep = std::current_exception()) {
        try {
            std::rethrow_exception(ep);
        } catch (const std::exception& e) {
            snprintf(buf, sizeof(buf), "std::terminate: %s", e.what());
        } catch (...) {
            snprintf(buf, sizeof(buf), "std::terminate: unknown exception type");
        }
    }
    writeLog("TERMINATE", buf);
    fflush(nullptr);
    std::abort();   // keep the default terminate behavior (process exits)
}

static void abortHandler(int) {
    writeLog("ABORT", "signal(SIGABRT) received (abort() / FFmpeg assert)");
    fflush(nullptr);
    std::signal(SIGABRT, SIG_DFL);
    std::abort();
}

// install all three crash handlers. appName is the prefix for the log/dump files.
static void install(const char* appName) {
    g_appName = appName;
    resolveDir();                       // resolve + create the target directory up front
    // keep the log from growing without bound on a machine that crashes every lesson
    std::string path = g_dir + g_appName + "_crash.log";
    WIN32_FILE_ATTRIBUTE_DATA fad{};
    if (GetFileAttributesExA(path.c_str(), GetFileExInfoStandard, &fad) &&
        fad.nFileSizeHigh == 0 && fad.nFileSizeLow > 1024u * 1024u) {
        DeleteFileA(path.c_str());
    }
    SetUnhandledExceptionFilter(sehHandler);
    std::set_terminate(terminateHandler);
    std::signal(SIGABRT, abortHandler);
    writeLog("RUN", "process started");   // proves the log path is writable before anything can crash
}

} // namespace crash_report
