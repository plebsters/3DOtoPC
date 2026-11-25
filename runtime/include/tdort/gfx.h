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
    friend struct StateIO;   // savestate.cpp
public:
    static constexpr int kWidth = 320, kHeight = 240;

    explicit Gfx(Memory& mem) : mem_(mem), frame_(kWidth * kHeight, 0) {}

    // lines: how many lines of the screen the display shows (its bitmap's
    // height); the lines below stay black, as on the console.
    void set_display_buffer(uint32_t addr, int lines = kHeight) { display_addr_ = addr; display_lines_ = lines; }
    int display_lines() const { return display_lines_; }
    uint32_t display_buffer() const { return display_addr_; }
    // Colour table from the screen's display list (VDL): kHeight lines of
    // 32 entries, each 0x00RRGGBB, giving the 8-bit level of each 5-bit
    // value per channel. Empty: the standard linear table.
    void set_display_clut(const std::vector<uint32_t>* clut) { clut_ = clut ? *clut : std::vector<uint32_t>(); }

    // Convert the guest framebuffer to XRGB8888.
    // 3DO bitmaps are 16bpp RGB555 stored in "LRform": each 32-bit word holds
    // the same column of two consecutive lines (even line in the high half).
    const uint32_t* scan_out();

    // Draw the CCB chain starting at ccb_addr. Returns the number of cels drawn.
    int draw_cels(const CelTarget& target, uint32_t ccb_addr);

    bool lrform = true;
    uint64_t cels_drawn = 0, pixels_written = 0;

    // ---- higher internal resolution ------------------------------------
    // Scale 2..4 draws every cel a second time into a hi-res copy of each
    // screen-sized bitmap (a "shadow"), so rotated, scaled and 3D cels get
    // smooth edges and full texture detail. The game's own 320x240 bitmaps
    // are drawn exactly as at scale 1: the game sees no difference. Pixels
    // the game writes itself (CPU, SPORT clears) are found by comparing the
    // bitmap with a copy of what the cel engine last left there, and are
    // copied into the shadow as blocks.
    void set_scale(int s);
    int  scale() const { return scale_; }
    // The picture to show: out_width() x out_height() (320*scale x 240*scale).
    // scan_out() must run first. At scale 1 it is the scan_out() picture.
    const uint32_t* out_frame() const { return scale_ > 1 ? hi_frame_.data() : frame_.data(); }
    int out_width() const { return kWidth * scale_; }
    int out_height() const { return kHeight * scale_; }
    // The game's side wrote guest memory [addr, addr+len) (SPORT): the
    // shadows take those pixels from the bitmap even if the value is the same.
    void note_write(uint32_t addr, uint32_t len);
    void drop_shadows() { shadows_.clear(); cur_ = -1; }

private:
    // CEL engine registers; fields a CCB does not load keep their last value.
    struct Regs {
        uint32_t flags = 0, pixc = 0x1F001F00, pre0 = 0, pre1 = 0;
        int32_t  xpos = 0, ypos = 0, hdx = 1 << 20, hdy = 0, vdx = 0, vdy = 1 << 16, hddx = 0, hddy = 0;
        uint16_t plut[32] = {};
    } r_;

    struct Pixel { uint16_t color; uint8_t pmode; uint8_t amv_r, amv_g, amv_b; bool transparent; bool msb; };

    void draw_one(const CelTarget& t, uint32_t source);
    Pixel decode(uint32_t raw, int bpp, bool uncoded) const;
    uint16_t process(const Pixel& p, uint16_t fb) const;
    void fill_quad(const CelTarget& t, const Pixel& px, const int32_t (&x)[4], const int32_t (&y)[4]);
    void plot(const CelTarget& t, int x, int y, const Pixel& px);

    struct Shadow {
        uint32_t buffer = 0;
        int w = 0, h = 0;                 // the bitmap's size (1x)
        std::vector<uint16_t> hi;         // (w*s) x (h*s), one pixel per entry, top to bottom
        std::vector<uint8_t> ref;         // the bitmap's bytes as the cel engine last left them
        uint64_t used = 0;
    };
    int  shadow_for(uint32_t buffer, int w, int h, bool create);
    void sync(Shadow& s);
    void fill_quad_hi(const CelTarget& t, const Pixel& px, const int32_t (&x)[4], const int32_t (&y)[4]);

    uint16_t fb_read(const CelTarget& t, int x, int y) const;
    void     fb_write(const CelTarget& t, int x, int y, uint16_t v);

    Memory& mem_;
    std::vector<uint32_t> frame_;
    int      display_lines_ = kHeight;
    uint32_t display_addr_ = 0;
    std::vector<uint32_t> clut_;

    int scale_ = 1;
    std::vector<Shadow> shadows_;
    int cur_ = -1;                        // shadow of the bitmap being drawn
    uint64_t use_clock_ = 0;
    std::vector<uint32_t> hi_frame_;
    std::vector<uint32_t> rgb_lut_;       // 16-bit pixel -> XRGB, without a colour table
};

} // namespace tdort
