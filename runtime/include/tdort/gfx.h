// tdort/gfx.h - 3DO video: framebuffer scan-out and a software CEL engine.
#pragma once
#include <cstdint>
#include <vector>
#include "memory.h"

namespace tdort {

struct Cpu;

// Where a DrawCels call renders: a bitmap in guest memory (LR-form 16 bpp).
struct CelTarget {
    uint32_t buffer = 0;
    int32_t  width = 320, height = 240;
    int32_t  clip_x = 0, clip_y = 0, clip_w = 320, clip_h = 240;
};

class Gfx {
public:
    static constexpr int kWidth = 320, kHeight = 240;

    explicit Gfx(Memory& mem) : mem_(mem), frame_(kWidth * kHeight, 0) {}

    void set_display_buffer(uint32_t addr) { display_addr_ = addr; }
    uint32_t display_buffer() const { return display_addr_; }

    // Convert the guest framebuffer to XRGB8888.
    // 3DO bitmaps are 16bpp RGB555 stored in "LRform": each 32-bit word holds
    // the same column of two consecutive lines (even line in the high half).
    const uint32_t* scan_out();

    // Draw the CCB chain starting at ccb_addr. Returns the number of cels drawn.
    int draw_cels(const CelTarget& target, uint32_t ccb_addr);

    bool lrform = true;
    uint64_t cels_drawn = 0, pixels_written = 0;

private:
    // CEL engine registers; fields a CCB does not load keep their last value.
    struct Regs {
        uint32_t flags = 0, pixc = 0x1F001F00, pre0 = 0, pre1 = 0;
        int32_t  xpos = 0, ypos = 0, hdx = 1 << 20, hdy = 0, vdx = 0, vdy = 1 << 16, hddx = 0, hddy = 0;
        uint16_t plut[32] = {};
    } r_;

    struct Pixel { uint16_t color; uint8_t pmode; uint8_t amv_r, amv_g, amv_b; bool transparent; };

    void draw_one(const CelTarget& t, uint32_t source);
    Pixel decode(uint32_t raw, int bpp, bool uncoded) const;
    uint16_t process(const Pixel& p, uint16_t fb) const;
    void fill_quad(const CelTarget& t, const Pixel& px, const int32_t (&x)[4], const int32_t (&y)[4]);
    void plot(const CelTarget& t, int x, int y, const Pixel& px);

    uint16_t fb_read(const CelTarget& t, int x, int y) const;
    void     fb_write(const CelTarget& t, int x, int y, uint16_t v);

    Memory& mem_;
    std::vector<uint32_t> frame_;
    uint32_t display_addr_ = 0;
};

} // namespace tdort
