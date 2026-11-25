// platform_null.cpp - headless platform for tests and non-Windows builds.
#include "tdort/platform.h"
#include <chrono>
#include <cstdlib>
#include <cstdio>
#include <set>
#include <string>
#include <thread>
#include <vector>

namespace tdort {

namespace {
struct NullPlatform : Platform {
    // headless runs stop after ~10 s of guest time (TDORT_FRAMES=n to change)
    int frames_left = std::getenv("TDORT_FRAMES") ? std::atoi(std::getenv("TDORT_FRAMES")) : 600;
    int frame = 0;
    bool scripted = std::getenv("TDORT_TEST_INPUT") != nullptr;
    std::set<int> snaps = parse_snaps();
    // TDORT_SNAP=120,300: write snap_<frame>.bmp for those frames (debugging).
    static std::set<int> parse_snaps() {
        std::set<int> out;
        if (const char* e = std::getenv("TDORT_SNAP")) {
            std::string s(e), cur;
            for (char ch : s + ",") { if (ch == ',') { if (!cur.empty()) out.insert(std::atoi(cur.c_str())); cur.clear(); } else cur += ch; }
        }
        return out;
    }
    static void write_bmp(const char* path, const uint32_t* px, int w, int h) {
        FILE* f = std::fopen(path, "wb");
        if (!f) return;
        const uint32_t row = uint32_t(w) * 3, pad = (4 - row % 4) % 4, size = 54 + (row + pad) * uint32_t(h);
        uint8_t hdr[54] = {'B', 'M'};
        auto put = [&](int off, uint32_t v) { for (int i = 0; i < 4; ++i) hdr[off + i] = uint8_t(v >> (8 * i)); };
        put(2, size); put(10, 54); put(14, 40); put(18, uint32_t(w)); put(22, uint32_t(h));
        hdr[26] = 1; hdr[28] = 24;
        std::fwrite(hdr, 1, 54, f);
        for (int y = h - 1; y >= 0; --y) {
            for (int x = 0; x < w; ++x) {
                uint32_t p = px[y * w + x];
                uint8_t b[3] = {uint8_t(p), uint8_t(p >> 8), uint8_t(p >> 16)};
                std::fwrite(b, 1, 3, f);
            }
            static const uint8_t z[3] = {0, 0, 0};
            std::fwrite(z, 1, pad, f);
        }
        std::fclose(f);
    }
    bool open(const char*, int, int) override { return true; }
    bool pump() override { ++frame; return frames_left-- > 0; }
    // TDORT_PRESS="1900-1910:P,2000-2005:A": hold buttons on frame ranges
    // (U D L R A B C P=play/start X=stop l r). For headless testing.
    struct Press { int from, to; uint32_t bits; };
    std::vector<Press> presses = parse_presses();
    static std::vector<Press> parse_presses() {
        std::vector<Press> out;
        const char* e = std::getenv("TDORT_PRESS");
        if (!e) return out;
        std::string s(e), item;
        for (char ch : s + ",") {
            if (ch != ',') { item += ch; continue; }
            auto colon = item.find(':'), dash = item.find('-');
            if (colon != std::string::npos) {
                Press p{};
                p.from = std::atoi(item.c_str());
                p.to = dash != std::string::npos && dash < colon ? std::atoi(item.c_str() + dash + 1) : p.from;
                for (char b : item.substr(colon + 1)) {
                    switch (b) {
                    case 'U': p.bits |= PadUp; break;   case 'D': p.bits |= PadDown; break;
                    case 'L': p.bits |= PadLeft; break; case 'R': p.bits |= PadRight; break;
                    case 'A': p.bits |= PadA; break;    case 'B': p.bits |= PadB; break;
                    case 'C': p.bits |= PadC; break;    case 'P': p.bits |= PadP; break;
                    case 'X': p.bits |= PadX; break;    case 'l': p.bits |= PadL; break;
                    case 'r': p.bits |= PadR; break;
                    }
                }
                out.push_back(p);
            }
            item.clear();
        }
        return out;
    }
    // TDORT_TEST_INPUT=1: hold A on frames 10-19 (used by the tests).
    uint32_t pad() override {
        uint32_t b = (scripted && frame >= 10 && frame < 20) ? PadA : 0;
        for (auto& p : presses) if (frame >= p.from && frame <= p.to) b |= p.bits;
        return b;
    }
    void present(const uint32_t* px, int w, int h) override {
        // Touch the picture's first and last pixel every frame, as the window
        // does, so a bad picture pointer fails headless tests too.
        static volatile uint32_t sink;
        sink = px[0] ^ px[size_t(w) * h - 1];
        if (snaps.count(frame)) { std::string n = "snap_" + std::to_string(frame) + ".bmp"; write_bmp(n.c_str(), px, w, h); }
    }
    // TDORT_WAV=file.wav records the game's sound (for checking audio headless).
    FILE* wav = nullptr;
    uint32_t wav_bytes = 0;
    void queue_audio(const int16_t* stereo, int frames) override {
        const char* path = std::getenv("TDORT_WAV");
        if (!path) return;
        if (!wav) {
            if (!(wav = std::fopen(path, "wb"))) return;
            uint8_t hdr[44] = {};
            std::fwrite(hdr, 1, 44, wav);
        }
        for (int i = 0; i < frames * 2; ++i) {   // little-endian 16-bit
            const uint16_t v = uint16_t(stereo[i]);
            std::fputc(v & 0xFF, wav); std::fputc(v >> 8, wav);
        }
        wav_bytes += uint32_t(frames) * 4;
    }
    ~NullPlatform() override {
        if (!wav) return;
        auto le32 = [](uint8_t* p, uint32_t v) { for (int i = 0; i < 4; ++i) p[i] = uint8_t(v >> (8 * i)); };
        uint8_t h[44] = {'R','I','F','F',0,0,0,0,'W','A','V','E','f','m','t',' ',16,0,0,0,1,0,2,0,
                         0,0,0,0,0,0,0,0,4,0,16,0,'d','a','t','a',0,0,0,0};
        le32(h + 4, 36 + wav_bytes); le32(h + 24, 44100); le32(h + 28, 44100 * 4); le32(h + 40, wav_bytes);
        std::fseek(wav, 0, SEEK_SET); std::fwrite(h, 1, 44, wav); std::fclose(wav);
    }
    void wait_frame(uint32_t) override {}
};
} // namespace

#if !defined(_WIN32)
std::unique_ptr<Platform> make_platform(bool) { return std::make_unique<NullPlatform>(); }
#else
std::unique_ptr<Platform> make_null_platform() { return std::make_unique<NullPlatform>(); }
#endif

} // namespace tdort
