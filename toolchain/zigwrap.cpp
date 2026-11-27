// zigwrap.cpp - the small zig-cc.exe / zig-c++.exe / zig-ar.exe / zig-ranlib.exe /
// zig-rc.exe front ends of the bundled compiler.
//
// CMake wants one program per tool, and zig is one program with sub-commands
// ("zig c++ ...", "zig ar ..."). Each copy of this program takes the tool from
// its own file name (zig-<tool>.exe), runs "zig.exe <tool> <arguments>" from
// its own folder with the arguments exactly as it got them, and returns zig's
// exit code. Get-Toolchain.ps1 builds one copy per tool with the downloaded
// zig, with the tool built in (Ninja may start a program by its 8.3 short name,
// such as ZIG-C_~1.EXE when the install path has spaces, so the file name is
// only the fallback, and then in its long form):
//   zig.exe c++ -O2 -s -DZIGWRAP_TOOL=1 -o zig-c++.exe zigwrap.cpp
// (1 c++, 2 cc, 3 ar, 4 ranlib, 5 rc: a number, so no quotes have to survive
// the command line).
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#include <string>

int main() {
    std::wstring self(32768, L'\0');
    const DWORD n = GetModuleFileNameW(nullptr, &self[0], DWORD(self.size()));
    if (n == 0 || n >= self.size()) return 127;
    self.resize(n);
    {   // ZIG-C_~1.EXE -> zig-c++.exe (and the folder names likewise)
        std::wstring lng(32768, L'\0');
        const DWORD m = GetLongPathNameW(self.c_str(), &lng[0], DWORD(lng.size()));
        if (m > 0 && m < lng.size()) { lng.resize(m); self = lng; }
    }
    const size_t slash = self.find_last_of(L"\\/");
    const std::wstring dir = self.substr(0, slash + 1), name = self.substr(slash + 1);
#ifdef ZIGWRAP_TOOL
    static const wchar_t* const kTools[] = {L"", L"c++", L"cc", L"ar", L"ranlib", L"rc"};
    static_assert(ZIGWRAP_TOOL >= 1 && ZIGWRAP_TOOL <= 5, "ZIGWRAP_TOOL: 1 c++, 2 cc, 3 ar, 4 ranlib, 5 rc");
    std::wstring tool = kTools[ZIGWRAP_TOOL];
    (void)name;
#else
    // zig-<tool>.exe -> <tool>
    if (name.size() < 9 || _wcsnicmp(name.c_str(), L"zig-", 4) != 0) return 127;
    std::wstring tool = name.substr(4);
    if (tool.size() > 4 && _wcsicmp(tool.c_str() + tool.size() - 4, L".exe") == 0) tool.resize(tool.size() - 4);
#endif
    const std::wstring zig = dir + L"zig.exe";

    // zig keeps the libraries it builds (the C runtime, libc++) in a cache:
    // the app's cache\ folder (<app>\toolchain\zig\ -> <app>\cache\zig), which
    // is writable and is removed on uninstall, unless one is set already.
    if (GetEnvironmentVariableW(L"ZIG_GLOBAL_CACHE_DIR", nullptr, 0) == 0) {
        const std::wstring app_cache = dir + L"..\\..\\cache";
        const DWORD attr = GetFileAttributesW(app_cache.c_str());
        if (attr != INVALID_FILE_ATTRIBUTES && (attr & FILE_ATTRIBUTE_DIRECTORY)) {
            const std::wstring zc = app_cache + L"\\zig";
            SetEnvironmentVariableW(L"ZIG_GLOBAL_CACHE_DIR", zc.c_str());
            SetEnvironmentVariableW(L"ZIG_LOCAL_CACHE_DIR", zc.c_str());
        }
    }

    // Everything after our own program name, untouched (quoting included).
    const wchar_t* p = GetCommandLineW();
    if (*p == L'"') { ++p; while (*p && *p != L'"') ++p; if (*p) ++p; }
    else while (*p && *p != L' ' && *p != L'\t') ++p;
    while (*p == L' ' || *p == L'\t') ++p;

    std::wstring cmd = L"\"" + zig + L"\" " + tool;
    if (*p) { cmd += L' '; cmd += p; }

    STARTUPINFOW si{};
    si.cb = sizeof si;
    PROCESS_INFORMATION pi{};
    if (!CreateProcessW(zig.c_str(), &cmd[0], nullptr, nullptr, TRUE, 0, nullptr, nullptr, &si, &pi)) return 127;
    WaitForSingleObject(pi.hProcess, INFINITE);
    DWORD code = 1;
    GetExitCodeProcess(pi.hProcess, &code);
    CloseHandle(pi.hThread);
    CloseHandle(pi.hProcess);
    return int(code);
}
