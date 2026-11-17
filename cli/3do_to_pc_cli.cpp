// 3do_to_pc_cli.cpp - "3DO to PC CLI" command-line front end (the GUI calls the same C API).
#include "tdo/api.h"
#include <cstdio>
#include <cstring>
#include <filesystem>
#include <string>
#include <vector>

static void on_log(int level, const char* msg, void*) {
    static const char* tag[3] = {"", "warning: ", "error: "};
    std::fprintf(level ? stderr : stdout, "%s%s\n", tag[level < 0 || level > 2 ? 0 : level], msg);
}

static void usage() {
    std::puts("usage:\n"
              "  \"3DO to PC CLI\" inspect <disc.iso>\n"
              "  \"3DO to PC CLI\" convert <disc.iso> <out_dir> [--name N] [--runtime DIR] [--hints FILE]\n"
              "                 [--launchme PATH] [--no-build] [--debug] [--no-scan]\n"
              "                 [--disc DISC2] [--disc DISC3] ...   (other discs of the game)\n"
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
        std::vector<std::string> discs;
        for (int i = 4; i < argc; ++i) {
            std::string a = argv[i];
            auto next = [&]() -> const char* { return i + 1 < argc ? argv[++i] : nullptr; };
            if (a == "--disc") { if (const char* d = next()) discs.push_back(d); }
            else if (a == "--name") o.game_name = next();
            else if (a == "--runtime") o.runtime_dir = next();
            else if (a == "--hints") o.hints_path = next();
            else if (a == "--launchme") o.launch_me = next();
            else if (a == "--no-build") o.build = 0;
            else if (a == "--debug") o.release = 0;
            else if (a == "--no-scan") o.scan_prologues = 0;
            else { usage(); return 1; }
        }
        // Disc folders left from an earlier conversion would count as extra
        // discs: remove those beyond the ones given this time.
        for (int k = int(discs.size()) + 2; k < 16; ++k) {
            const std::filesystem::path stale = std::filesystem::u8path(std::string(o.output_dir) + "/data_disc" + std::to_string(k));
            std::error_code ec;
            if (std::filesystem::exists(stale, ec)) {
                std::filesystem::remove_all(stale, ec);
                std::printf("Removed %s (from an earlier conversion)\n", stale.u8string().c_str());
            }
        }
        // Other discs first: unpacking them takes seconds, the build minutes.
        for (size_t k = 0; k < discs.size(); ++k) {
            const std::string dest = std::string(o.output_dir) + "/data_disc" + std::to_string(k + 2);
            std::printf("Disc %zu: %s\n", k + 2, discs[k].c_str());
            if (int rc = tdo_extract_disc(discs[k].c_str(), dest.c_str(), on_log, nullptr)) return rc;
        }
        return tdo_convert(&o, on_log, nullptr, nullptr);
    }
    usage();
    return 1;
}
