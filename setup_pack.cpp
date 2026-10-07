// setup_pack.cpp -- single-file installer package shell (self-extracting launcher, pure Win32, zero deps)
// packaging (see pack.ps1): setup_pack.exe + payload files (installer/exe/dll concatenated in order) + trailing index
// at runtime: parse the trailing index -> extract payload by offset to a temp dir -> auto-run installer.exe -> clean up and exit
// command: setup_pack.exe /e <dir>   only extract to the given dir then exit (for packaging verification)
#include <winsock2.h>
#include <windows.h>
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>
#include <algorithm>
#include <filesystem>
#pragma comment(linker, "/SUBSYSTEM:WINDOWS /ENTRY:mainCRTStartup")

namespace fs = std::filesystem;

// trailing layout (backwards from the file end): [magic:8 "SCPK1001"][count:u32][entriesBytes:u32][entries][payload...]
// each entry in the list: offset:u64 + size:u64 + nameLen:u16 + name
static const char kMagic[9] = "SCPK1001";

#pragma pack(push, 1)
struct PakEntry { uint64_t offset; uint64_t size; uint16_t nameLen; };
#pragma pack(pop)

struct Entry { std::string name; uint64_t off, size; };

// path-traversal guard: the entry name comes from the package's trailing index and must be a plain file
// name only (no directory separators / drive letters / ".."), so a tampered package can't write outside destDir
static bool isSafeEntryName(const std::string& n) {
    if (n.empty() || n == "." || n == "..") return false;
    for (unsigned char c : n) {
        if (c == '/' || c == '\\' || c == ':' || c < 0x20) return false;
    }
    return true;
}

static bool readAt(HANDLE f, int64_t pos, void* buf, int32_t len) {
    LARGE_INTEGER li; li.QuadPart = pos;
    if (!SetFilePointerEx(f, li, nullptr, FILE_BEGIN)) return false;
    DWORD rd = 0;
    return ReadFile(f, buf, (DWORD)len, &rd, nullptr) != 0 && rd == (DWORD)len;
}

static bool parseFooter(HANDLE f, std::vector<Entry>& entries, int64_t fileSize) {
    if (fileSize < 20) return false;
    char magic[9] = {};
    if (!readAt(f, fileSize - 8, magic, 8)) return false;
    if (memcmp(magic, kMagic, 8) != 0) return false;
    uint32_t count = 0, entriesBytes = 0;
    if (!readAt(f, fileSize - 12, &count, 4)) return false;
    if (!readAt(f, fileSize - 16, &entriesBytes, 4)) return false;
    int64_t base = fileSize - 16 - (int64_t)entriesBytes;
    if (base < 0 || entriesBytes > 1u << 20) return false;   // index cap 1MB, prevent corrupt files
    std::vector<uint8_t> buf(entriesBytes);
    if (!readAt(f, base, buf.data(), (int32_t)entriesBytes)) return false;
    const uint8_t* p = buf.data();
    const uint8_t* end = buf.data() + buf.size();
    for (uint32_t i = 0; i < count; ++i) {
        if (end - p < (int)sizeof(PakEntry)) return false;
        PakEntry e;
        memcpy(&e, p, sizeof(e)); p += sizeof(e);
        if (e.nameLen == 0 || end - p < e.nameLen || e.nameLen > 260) return false;
        Entry ent;
        ent.off = e.offset; ent.size = e.size;
        ent.name.assign((const char*)p, e.nameLen);
        p += e.nameLen;
        if (!isSafeEntryName(ent.name)) return false;   // reject the whole package on any unsafe name
        entries.push_back(std::move(ent));
    }
    return true;
}

static bool extractOne(HANDLE self, const Entry& e, const std::wstring& destDir) {
    if (!isSafeEntryName(e.name)) return false;   // defense in depth: never write outside destDir
    std::wstring path = destDir + L"\\" + std::wstring(e.name.begin(), e.name.end());
    HANDLE out = CreateFileW(path.c_str(), GENERIC_WRITE, 0, nullptr, CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
    if (out == INVALID_HANDLE_VALUE) return false;
    bool ok = true;
    const size_t CHUNK = 4 * 1024 * 1024;
    std::vector<uint8_t> buf((size_t)std::min<uint64_t>((uint64_t)CHUNK, e.size));
    uint64_t done = 0;
    while (done < e.size) {
        LARGE_INTEGER li; li.QuadPart = (LONGLONG)(e.off + done);
        if (!SetFilePointerEx(self, li, nullptr, FILE_BEGIN)) { ok = false; break; }
        DWORD want = (DWORD)std::min<uint64_t>(buf.size(), e.size - done);
        DWORD rd = 0;
        if (!ReadFile(self, buf.data(), want, &rd, nullptr) || rd != want) { ok = false; break; }
        DWORD wr = 0;
        if (!WriteFile(out, buf.data(), want, &wr, nullptr) || wr != want) { ok = false; break; }
        done += want;
    }
    CloseHandle(out);
    if (!ok) DeleteFileW(path.c_str());
    return ok;
}

static bool extractAll(HANDLE self, const std::vector<Entry>& entries, const std::wstring& dir) {
    std::error_code ec;
    fs::create_directories(dir, ec);
    for (const auto& x : fs::directory_iterator(dir, ec)) fs::remove_all(x.path(), ec);
    bool ok = true;
    for (const auto& e : entries)
        ok &= extractOne(self, e, dir);
    if (!ok) { fs::remove_all(dir, ec); return false; }
    return true;
}

int main(int argc, char* argv[]) {
    SetProcessDPIAware();
    wchar_t selfPath[MAX_PATH];
    GetModuleFileNameW(nullptr, selfPath, MAX_PATH);
    HANDLE self = CreateFileW(selfPath, GENERIC_READ, FILE_SHARE_READ, nullptr, OPEN_EXISTING, 0, nullptr);
    if (self == INVALID_HANDLE_VALUE) return 1;
    LARGE_INTEGER sz;
    GetFileSizeEx(self, &sz);
    std::vector<Entry> entries;
    if (!parseFooter(self, entries, sz.QuadPart)) { CloseHandle(self); return 1; }

    // /e <dir>: extract-only verification mode
    if (argc >= 3 && (_stricmp(argv[1], "/e") == 0 || _stricmp(argv[1], "-e") == 0)) {
        std::wstring dir(argv[2], argv[2] + strlen(argv[2]));
        bool ok = extractAll(self, entries, dir);
        CloseHandle(self);
        return ok ? 0 : 1;
    }

    // normal mode: extract to a temp dir and run installer.exe
    wchar_t tmp[MAX_PATH];
    GetTempPathW(MAX_PATH, tmp);
    std::wstring dir = std::wstring(tmp) + L"ScreenCast_setup_" + std::to_wstring(GetCurrentProcessId());
    std::error_code ec;
    fs::remove_all(dir, ec);
    if (!extractAll(self, entries, dir)) { CloseHandle(self); return 1; }
    CloseHandle(self);

    std::wstring inst = dir + L"\\installer.exe";
    STARTUPINFOW si{ sizeof(si) };
    PROCESS_INFORMATION pi{};
    if (CreateProcessW(inst.c_str(), nullptr, nullptr, nullptr, FALSE, 0, nullptr, dir.c_str(), &si, &pi)) {
        WaitForSingleObject(pi.hProcess, INFINITE);
        DWORD code = 0;
        GetExitCodeProcess(pi.hProcess, &code);
        CloseHandle(pi.hThread); CloseHandle(pi.hProcess);
        fs::remove_all(dir, ec);
        return (int)code;
    }
    fs::remove_all(dir, ec);
    return 1;
}