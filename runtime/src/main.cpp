// main.cpp - entry point of every converted game executable.
#include "tdort/runtime.h"
#include "tdort/platform.h"
#include <cstring>
#include <string>

int main(int argc, char** argv) {
    tdort::RuntimeConfig cfg;
    bool headless = false;
    for (int i = 1; i < argc; ++i) {
        if (!std::strcmp(argv[i], "--interp")) cfg.interp_only = true;
        else if (!std::strcmp(argv[i], "--trace")) cfg.trace = true;
        else if (!std::strcmp(argv[i], "--headless")) headless = true;
        else if (!std::strcmp(argv[i], "--data") && i + 1 < argc) cfg.data_dir = argv[++i];
    }
    tdort::Runtime rt(cfg);
    std::string err;
    if (!rt.load_game(err)) { rt.log("load failed: %s", err.c_str()); return 1; }
    auto platform = tdort::make_platform(headless);
    if (!platform->open(tdort::tdort_game_name, 320, 240)) { rt.log("cannot open window"); return 1; }
    return rt.run(platform.get());
}

#if defined(_WIN32)
#include <windows.h>
int WINAPI WinMain(HINSTANCE, HINSTANCE, LPSTR, int) { return main(__argc, __argv); }
#endif
