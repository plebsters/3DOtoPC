// tdort/platform.h - host OS layer (window, input, audio output, timing).
#pragma once
#include <cstdint>
#include <memory>

namespace tdort {

// 3DO control pad bits (host-side mapping; HLE translates for the game).
enum PadBit : uint32_t {
    PadUp = 1 << 0, PadDown = 1 << 1, PadLeft = 1 << 2, PadRight = 1 << 3,
    PadA = 1 << 4, PadB = 1 << 5, PadC = 1 << 6,
    PadL = 1 << 7, PadR = 1 << 8, PadX = 1 << 9 /*Stop*/, PadP = 1 << 10 /*Play/Pause*/,
};

struct Platform {
    virtual ~Platform() = default;
    virtual bool     open(const char* title, int width, int height) = 0;
    virtual bool     pump() = 0;                      // false when the user closed the window
    virtual uint32_t pad() = 0;                       // PadBit mask
    // 32-bit XRGB frame, width*height pixels.
    virtual void     present(const uint32_t* pixels, int width, int height) = 0;
    virtual void     queue_audio(const int16_t* stereo, int frames) = 0;
    virtual void     wait_frame(uint32_t hz) = 0;     // frame pacing
    virtual void     fatal(const char* /*message*/) {}  // tell the user the game stopped
};

std::unique_ptr<Platform> make_platform(bool headless);

} // namespace tdort
