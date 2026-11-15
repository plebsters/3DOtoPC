// 3do_to_pc_cli.cpp - "3DO to PC CLI" command-line front end (the GUI calls the same C API).
#include "tdo/api.h"
#include <cstdio>
#include <cstring>
#include <string>

static void on_log(int level, const char* msg, void*) {
    static const char* tag[3] = {"", "warning: ", "error: "};
    std::fprintf(level ? stderr : stdout, "%s%s\n", tag[level < 0 || level > 2 ? 0 : level], msg);
}

static void usage() {
    std::puts("usage:\n"
              "  \"3DO to PC CLI\" inspect <disc.iso>\n"
              "  \"3DO to PC CLI\" convert <disc.iso> <out_dir> [--name N] [--runtime DIR] [--hints FILE]\n"
              "                 [--launchme PATH] [--no-build] [--debug] [--no-scan]\n"
              "  \"3DO to PC CLI\" toolchain");
}

int main(int argc, char** argv) {
    if (argc < 2) { usage(); return 1; }
    const std::string cmd = argv[1];
    if (cmd == "inspect" && argc >= 3) return tdo_inspect(argv[2], on_log, nullptr);
    if (cmd == "toolchain") {
        char buf[1024]; int rc = tdo_detect_toolchain(buf, sizeof buf);
        std::puts(buf); return rc;
    }
    if (cmd == "convert" && argc >= 4) {
        tdo_convert_options o{};
        o.disc_path = argv[2]; o.output_dir = argv[3];
        o.build = 1; o.scan_prologues = 1; o.release = 1;
        for (int i = 4; i < argc; ++i) {
            std::string a = argv[i];
            auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
            if (a == "--name") o.game_name = next();
            else if (a == "--runtime") o.runtime_dir = next();
            else if (a == "--hints") o.hints_path = next();
            else if (a == "--launchme") o.launch_me = next();
            else if (a == "--no-build") o.build = 0;
            else if (a == "--debug") o.release = 0;
            else if (a == "--no-scan") o.scan_prologues = 0;
            else { usage(); return 1; }
        }
        return tdo_convert(&o, on_log, nullptr, nullptr);
    }
    usage();
    return 1;
}
