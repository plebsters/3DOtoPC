// toolchain.cpp - local compiler discovery and process execution.
#include "tdo/toolchain.h"
#include <cstdio>
#include <cstdlib>
#include <filesystem>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <unistd.h>
#include <climits>
#endif

namespace fs = std::filesystem;

namespace tdo {

std::string module_dir() {
#if defined(_WIN32)
    HMODULE h = nullptr;
    GetModuleHandleExW(GET_MODULE_HANDLE_EX_FLAG_FROM_ADDRESS | GET_MODULE_HANDLE_EX_FLAG_UNCHANGED_REFCOUNT,
                       reinterpret_cast<LPCWSTR>(&module_dir), &h);
    wchar_t buf[MAX_PATH]; GetModuleFileNameW(h, buf, MAX_PATH);
    return fs::path(buf).parent_path().u8string();
#else
    char buf[PATH_MAX]; ssize_t n = readlink("/proc/self/exe", buf, sizeof buf - 1);
    if (n <= 0) return ".";
    buf[n] = 0;
    return fs::path(buf).parent_path().string();
#endif
}

static std::string quote(const std::string& s) { return "\"" + s + "\""; }

int run_command(const std::string& cmdline, const std::string& cwd,
                const std::function<void(const std::string&)>& on_line) {
#if defined(_WIN32)
    // cmd /c needs the whole line wrapped in an extra pair of quotes.
    std::string full = "\"cd /d " + quote(cwd) + " && " + cmdline + " 2>&1\"";
    FILE* p = _popen(full.c_str(), "r");
#else
    std::string full = "cd " + quote(cwd) + " && " + cmdline + " 2>&1";
    FILE* p = popen(full.c_str(), "r");
#endif
    if (!p) return -1;
    char line[4096];
    while (std::fgets(line, sizeof line, p)) {
        std::string s(line);
        while (!s.empty() && (s.back() == '\n' || s.back() == '\r')) s.pop_back();
        if (on_line) on_line(s);
    }
#if defined(_WIN32)
    return _pclose(p);
#else
    int st = pclose(p);
    return WIFEXITED(st) ? WEXITSTATUS(st) : -1;
#endif
}

Toolchain detect_toolchain() {
    Toolchain t;
    const fs::path app = fs::u8path(module_dir());

#if defined(_WIN32)
    // 1. Portable toolchain shipped next to the app (the installer includes
    //    it): zig (clang with the MinGW-w64 headers and libraries), CMake and
    //    Ninja. Nothing from Visual Studio or the Windows SDK is needed, and
    //    the games it builds use only DLLs that come with Windows 10 and 11.
    //    zig-windows.cmake points CMake at the small zig-*.exe front ends
    //    (zig-c++.exe runs "zig c++ ...", and so on).
    fs::path bundled = app / "toolchain";
    fs::path b_cmake = bundled / "cmake" / "bin" / "cmake.exe";
    fs::path b_ninja = bundled / "ninja" / "ninja.exe";
    fs::path b_zig   = bundled / "zig" / "zig.exe";
    fs::path b_file  = bundled / "zig-windows.cmake";
    if (!std::getenv("TDO_NO_BUNDLED_TOOLCHAIN") && fs::exists(b_cmake) && fs::exists(b_ninja) &&
        fs::exists(b_zig) && fs::exists(bundled / "zig" / "zig-c++.exe") && fs::exists(b_file)) {
        t.found = true;
        t.description = "Bundled zig/clang (" + bundled.u8string() + ")";
        t.cmake = b_cmake.u8string();
        t.generator = "Ninja";
        t.configure_args = {
            "-DCMAKE_MAKE_PROGRAM=" + quote(b_ninja.generic_u8string()),
            "-DCMAKE_TOOLCHAIN_FILE=" + quote(b_file.generic_u8string()),
            "--no-warn-unused-cli",   // the projects are C++ only: no "CMAKE_C_COMPILER not used" warning
        };
        return t;
    }

    // 2. Visual Studio or Build Tools via vswhere.
    const char* pf86 = std::getenv("ProgramFiles(x86)");
    fs::path vswhere = fs::u8path(pf86 ? pf86 : "C:\\Program Files (x86)") /
                       "Microsoft Visual Studio" / "Installer" / "vswhere.exe";
    if (fs::exists(vswhere)) {
        std::string install;
        auto vsw = [&](const std::string& args, std::string& out) {
            out.clear();
            run_command(quote(vswhere.u8string()) + " " + args, app.u8string(),
                        [&](const std::string& l) { if (out.empty() && !l.empty()) out = l; });
        };
        // Prefer an install that has both the C++ compiler and the CMake tools.
        vsw("-latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 "
            "Microsoft.VisualStudio.Component.VC.CMake.Project -property installationPath", install);
        if (install.empty())
            vsw("-latest -products * -requires Microsoft.VisualStudio.Component.VC.Tools.x86.x64 "
                "-property installationPath", install);
        if (!install.empty()) {
            fs::path vs = fs::u8path(install);
            fs::path vcvars = vs / "VC" / "Auxiliary" / "Build" / "vcvars64.bat";
            fs::path cm = vs / "Common7" / "IDE" / "CommonExtensions" / "Microsoft" / "CMake";
            fs::path cmake = cm / "CMake" / "bin" / "cmake.exe";
            fs::path ninja = cm / "Ninja" / "ninja.exe";
            // CMake/Ninja may live in a different Visual Studio install than the compiler.
            const std::string rel = "Common7\\IDE\\CommonExtensions\\Microsoft\\CMake\\";
            std::string found;
            if (!fs::exists(cmake)) {
                vsw("-all -products * -find \"" + rel + "CMake\\bin\\cmake.exe\"", found);
                if (!found.empty()) cmake = fs::u8path(found);
                else if (const char* pf = std::getenv("ProgramFiles"))
                    if (fs::exists(fs::u8path(pf) / "CMake" / "bin" / "cmake.exe")) cmake = fs::u8path(pf) / "CMake" / "bin" / "cmake.exe";
            }
            if (!fs::exists(ninja)) {
                vsw("-all -products * -find \"" + rel + "Ninja\\ninja.exe\"", found);
                if (!found.empty()) ninja = fs::u8path(found);
            }
            if (fs::exists(vcvars) && fs::exists(cmake)) {
                t.found = true;
                t.cmake = cmake.u8string();
                t.env_setup = vcvars.u8string();
                if (fs::exists(ninja)) {
                    t.description = "Visual Studio (" + install + ")";
                    t.generator = "Ninja";
                    t.configure_args = {"-DCMAKE_MAKE_PROGRAM=" + quote(ninja.generic_u8string()),
                                        "-DCMAKE_C_COMPILER=cl", "-DCMAKE_CXX_COMPILER=cl", "--no-warn-unused-cli"};
                } else {
                    // NMake ships with the C++ tools, so it works whenever cl does.
                    t.description = "Visual Studio, NMake (" + install + ")";
                    t.generator = "NMake Makefiles";
                }
                return t;
            }
        }
    }
#endif

    // 3. Whatever is on PATH.
    int rc = run_command("cmake --version", app.u8string(), nullptr);
    if (rc == 0) {
        t.found = true;
        t.description = "cmake on PATH (system compiler)";
        t.cmake = "cmake";
        if (run_command("ninja --version", app.u8string(), nullptr) == 0) t.generator = "Ninja";
        return t;
    }
    t.description = "No compiler found. Reinstall 3DO to PC (its installer includes the compiler), "
                    "or install Visual Studio Build Tools (C++ workload).";
    return t;
}

} // namespace tdo
