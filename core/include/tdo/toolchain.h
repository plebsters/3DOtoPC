// tdo/toolchain.h - discovers a compiler already installed on this PC.
// Search order (all local, nothing is downloaded):
//   1. <app dir>/toolchain/  - portable CMake + Ninja + LLVM shipped with the app
//   2. Visual Studio / Build Tools found through vswhere.exe (MSVC)
//   3. cmake + a compiler on PATH
#pragma once
#include <functional>
#include <string>
#include <vector>

namespace tdo {

struct Toolchain {
    bool        found = false;
    std::string description;
    std::string cmake;                // path to cmake executable
    std::string generator;            // "Ninja" or "Visual Studio 17 2022" ...
    std::vector<std::string> configure_args;   // extra -D args (compiler paths)
    std::string env_setup;            // optional vcvars batch to call first (Windows)
};

std::string module_dir();             // directory of the running exe / dll
Toolchain   detect_toolchain();

// Runs a command line, streaming output lines. Returns the exit code.
int run_command(const std::string& cmdline, const std::string& cwd,
                const std::function<void(const std::string&)>& on_line);

} // namespace tdo
