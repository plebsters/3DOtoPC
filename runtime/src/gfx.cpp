// gfx.cpp - framebuffer scan-out and a software implementation of the 3DO
// CEL engine (data unpacker, pixel decoder, pixel processor, projector).
#include "tdort/gfx.h"
#include <algorithm>
#include <cstdlib>
#include <cstdio>

namespace tdort {

// CCB flag bits
enum : uint32_t {
    kSkip = 0x80000000, kLast = 0x40000000, kNpAbs = 0x20000000, kSpAbs = 0x10000000, kPpAbs = 0x08000000,
    kLdSize = 0x04000000, kLdPrs = 0x02000000, kLdPpmp = 0x01000000, kLdPlut = 0x00800000, kCcbPre = 0x00400000,
    kYoxy = 0x00200000,
    kAcw = 0x00040000, kAccw = 0x00020000, kPxor = 0x00000800, kPacked = 0x00000200, kPlutPos = 0x00000040,
    kBgnd = 0x00000020, kNoBlk = 0x00000010,
};
// Preamble bits
enum : uint32_t { kPre0Bgnd = 0x40000000, kPre0Uncoded = 0x00000010, kPre0Rep8 = 0x00000008, kPre1LrForm = 0x00000800 };

static inline uint32_t rd32(const Memory& m, uint32_t a) { return a < kRamEnd - 3 ? load_be32(&m.ram[a]) : 0; }
static inline uint16_t rd16(const Memory& m, uint32_t a) {
    return a < kRamEnd - 1 ? uint16_t((m.ram[a] << 8) | m.ram[a + 1]) : 0;
}

// Big-endian bit reader over guest memory.
static inline uint32_t bits_at(const Memory& m, uint64_t bitpos, int n) {
    uint32_t v = 0;
    for (int i = 0; i < n; ++i, ++bitpos) {
        uint32_t byte = uint32_t(bitpos >> 3);
        uint32_t b = byte < kRamEnd ? m.ram[byte] : 0;
        v = (v << 1) | ((b >> (7 - (bitpos & 7))) & 1);
    }
    return v;
}

static inline uint32_t rgb555_to_xrgb(uint32_t p) {
    uint32_t r = (p >> 10) & 31, g = (p >> 5) & 31, b = p & 31;
    return (r << 19 | (r >> 2) << 16) | (g << 11 | (g >> 2) << 8) | (b << 3 | b >> 2);
}

const uint32_t* Gfx::scan_out() {
    if (display_addr_ == 0 || display_addr_ >= kRamEnd) return frame_.data();
    const uint8_t* src = &mem_.ram[display_addr_];
    const uint32_t bytes = kWidth * kHeight * 2;
    if (display_addr_ > kRamEnd - bytes) return frame_.data();
    for (int y = 0; y < kHeight; ++y) {
        for (int x = 0; x < kWidth; ++x) {
            uint32_t pix;
            if (lrform) {
                const uint8_t* w = src + ((y >> 1) * kWidth + x) * 4;
                uint32_t word = load_be32(w);
                pix = (y & 1) ? (word & 0xFFFF) : (word >> 16);
            } else {
                const uint8_t* p = src + (y * kWidth + x) * 2;
                pix = (uint32_t(p[0]) << 8) | p[1];
            }
            frame_[y * kWidth + x] = rgb555_to_xrgb(pix & 0x7FFF);
        }
    }
    return frame_.data();
}

// ---- framebuffer access (LR form: line pairs share 32-bit words) -----------

uint16_t Gfx::fb_read(const CelTarget& t, int x, int y) const {
    uint32_t a = t.buffer + uint32_t(((y >> 1) * t.width + x) * 4 + ((y & 1) ? 2 : 0));
    return rd16(mem_, a);
}

void Gfx::fb_write(const CelTarget& t, int x, int y, uint16_t v) {
    uint32_t a = t.buffer + uint32_t(((y >> 1) * t.width + x) * 4 + ((y & 1) ? 2 : 0));
    if (a >= kRamEnd - 1) return;   // also catches addresses that wrap past 4 GB
    mem_.ram[a] = uint8_t(v >> 8);
    mem_.ram[a + 1] = uint8_t(v);
}

// ---- pixel decoder ----------------------------------------------------------

Gfx::Pixel Gfx::decode(uint32_t raw, int bpp, bool uncoded) const {
    Pixel p{};
    const uint32_t flags = r_.flags;
    if (uncoded && bpp == 16) {
        p.color = uint16_t(raw & 0x7FFF);
        p.pmode = uint8_t((raw >> 15) & 1);
    } else if (uncoded && bpp == 8) {
        // RRRGGGBB unfolded to 5:5:5, optionally replicating high bits (REP8)
        const bool rep = r_.pre0 & kPre0Rep8;
        uint32_t r3 = (raw >> 5) & 7, g3 = (raw >> 2) & 7, b2 = raw & 3;
        uint32_t r5 = (r3 << 2) | (rep ? (r3 >> 1) : 0);
        uint32_t g5 = (g3 << 2) | (rep ? (g3 >> 1) : 0);
        uint32_t bh = b2 >> 1, bl = b2 & 1;
        uint32_t b5 = (b2 << 3) | (rep ? ((bh << 2) | (bl << 1) | bh) : 0);
        p.color = uint16_t((r5 << 10) | (g5 << 5) | b5);
    } else {
        // Coded: index the PLUT. Pixels with fewer than 5 bits take the
        // missing high index bits from the CCB's PLUTA field.
        uint32_t idx;
        bool pmode_from_plut = true;
        switch (bpp) {
        case 1: case 2: case 4: {
            uint32_t mask = (1u << bpp) - 1;
            idx = (((flags & 0xF) << 1) & 0x1E & ~mask) | (raw & mask);
            break;
        }
        case 6:
            idx = raw & 0x1F; p.pmode = uint8_t((raw >> 5) & 1); pmode_from_plut = false;
            break;
        case 8:
            idx = raw & 0x1F;
            p.amv_r = p.amv_g = p.amv_b = uint8_t(((raw >> 5) & 7) + 1);
            break;
        default:   // 16-bit coded: per-channel multipliers and P-mode in the pixel
            idx = raw & 0x1F;
            p.amv_b = uint8_t(((raw >> 5) & 7) + 1);
            p.amv_g = uint8_t(((raw >> 8) & 7) + 1);
            p.amv_r = uint8_t(((raw >> 11) & 7) + 1);
            p.pmode = uint8_t((raw >> 15) & 1); pmode_from_plut = false;
            break;
        }
        const uint16_t v = r_.plut[idx & 31];
        p.color = uint16_t(v & 0x7FFF);
        if (pmode_from_plut) p.pmode = uint8_t(v >> 15);
    }
    // P-mode override from the CCB (POVER)
    switch ((flags >> 7) & 3) {
    case 2: p.pmode = 0; break;
    case 3: p.pmode = 1; break;
    default: break;
    }
    p.transparent = p.color == 0 && !(r_.pre0 & kPre0Bgnd) && !(flags & kBgnd);
    return p;
}

// ---- pixel processor (per 5-bit channel) --------------------------------------

uint16_t Gfx::process(const Pixel& p, uint16_t fb) const {
    const uint32_t ctl = p.pmode ? (r_.pixc >> 16) & 0xFFFF : r_.pixc & 0xFFFF;
    uint16_t out;
    if (ctl == 0x1F40 || ctl == 0x1F00) {
        out = p.color;          // normal mode: cel colour unchanged
    } else {
        const bool prim_fb = ctl & 0x8000;
        const uint32_t ms = (ctl >> 13) & 3, mf = ((ctl >> 10) & 7) + 1, sf = (ctl >> 8) & 3;
        const uint32_t div = sf == 0 ? 16 : (1u << sf);
        const uint32_t s2 = (ctl >> 6) & 3, av = (ctl >> 1) & 0x1F;
        const bool two_d = ctl & 1;
        const bool invert = av & 1, wrap = av & 4;
        const uint32_t sdv_code = (av >> 3) & 3;
        const uint32_t sdv = sdv_code == 1 ? 2 : sdv_code == 2 ? 4 : 1;
        const uint8_t amv[3] = {p.amv_r, p.amv_g, p.amv_b};
        out = 0;
        for (int ch = 0; ch < 3; ++ch) {
            const int shift = 10 - ch * 5;
            const int32_t pc = (p.color >> shift) & 31, fc = (fb >> shift) & 31;
            const int32_t prim = prim_fb ? fc : pc;
            int32_t mult;
            if (ms == 0) mult = int32_t(mf);
            else if (ms == 1) mult = amv[ch] ? amv[ch] : int32_t(mf);
            else mult = (pc >> 2) + 1;
            const int32_t a = prim * mult / int32_t(div);
            int32_t sec = 0;
            if (s2 == 1) sec = int32_t(av);
            else if (s2 == 2) sec = fc / int32_t(sdv);
            else if (s2 == 3) sec = pc / int32_t(sdv);
            int32_t res;
            if (r_.flags & kPxor) res = a ^ sec;
            else if (s2 >= 2 && invert) res = a - sec;
            else res = a + sec;
            if (two_d) res >>= 1;
            res = wrap ? (res & 31) : std::clamp(res, 0, 31);
            out = uint16_t(out | (res << shift));
        }
    }
    if (out == 0 && !(r_.flags & kNoBlk)) out = 0x0400;   // keep "black" distinct from background
    return out;
}

// ---- projector --------------------------------------------------------------

void Gfx::plot(const CelTarget& t, int x, int y, const Pixel& px) {
    if (x < t.clip_x || y < t.clip_y || x >= t.clip_x + t.clip_w || y >= t.clip_y + t.clip_h) return;
    if (x < 0 || y < 0 || x >= t.width || y >= t.height) return;
    const uint32_t ctl = px.pmode ? (r_.pixc >> 16) & 0xFFFF : r_.pixc & 0xFFFF;
    const bool needs_fb = (ctl & 0x8000) || ((ctl >> 6) & 3) == 2;
    const uint16_t fb = needs_fb ? fb_read(t, x, y) : 0;
    fb_write(t, x, y, process(px, fb));
    ++pixels_written;
}

// Corners in 16.16, order TL, TR, BR, BL. Fills pixels whose centres are inside.
void Gfx::fill_quad(const CelTarget& t, const Pixel& px, const int32_t (&x)[4], const int32_t (&y)[4]) {
    // Face culling: a mirrored/rotated-away pixel has the opposite winding.
    const int64_t area = int64_t(x[1] - x[0]) * (y[2] - y[0]) - int64_t(y[1] - y[0]) * (x[2] - x[0]);
    if (area == 0) return;
    if (area > 0 && !(r_.flags & kAcw)) return;
    if (area < 0 && !(r_.flags & kAccw)) return;

    // Axis-aligned fast path (the common unrotated sprite)
    if (y[0] == y[1] && y[2] == y[3] && x[0] == x[3] && x[1] == x[2]) {
        int32_t xa = std::min(x[0], x[1]), xb = std::max(x[0], x[1]);
        int32_t ya = std::min(y[0], y[3]), yb = std::max(y[0], y[3]);
        // pixel n is covered if its centre n+0.5 lies in [a, b)
        int px0 = int((int64_t(xa) + 0x7FFF) >> 16), px1 = int((int64_t(xb) + 0x7FFF) >> 16);
        int py0 = int((int64_t(ya) + 0x7FFF) >> 16), py1 = int((int64_t(yb) + 0x7FFF) >> 16);
        for (int yy = py0; yy < py1; ++yy)
            for (int xx = px0; xx < px1; ++xx) plot(t, xx, yy, px);
        return;
    }
    int32_t minx = std::min({x[0], x[1], x[2], x[3]}), maxx = std::max({x[0], x[1], x[2], x[3]});
    int32_t miny = std::min({y[0], y[1], y[2], y[3]}), maxy = std::max({y[0], y[1], y[2], y[3]});
    int bx0 = std::max<int>(int(minx >> 16), t.clip_x), bx1 = std::min<int>(int(maxx >> 16) + 1, t.clip_x + t.clip_w);
    int by0 = std::max<int>(int(miny >> 16), t.clip_y), by1 = std::min<int>(int(maxy >> 16) + 1, t.clip_y + t.clip_h);
    const int sign = area > 0 ? 1 : -1;
    for (int yy = by0; yy < by1; ++yy) {
        const int64_t cy = (int64_t(yy) << 16) + 0x8000;
        for (int xx = bx0; xx < bx1; ++xx) {
            const int64_t cx = (int64_t(xx) << 16) + 0x8000;
            bool inside = true;
            for (int e = 0; e < 4 && inside; ++e) {
                const int f = (e + 1) & 3;
                const int64_t ex = int64_t(x[f]) - x[e], ey = int64_t(y[f]) - y[e];
                const int64_t cross = ex * (cy - y[e]) - ey * (cx - x[e]);
                if (cross * sign < 0) inside = false;
            }
            if (inside) plot(t, xx, yy, px);
        }
    }
}

void Gfx::draw_one(const CelTarget& t, uint32_t src) {
    const uint32_t flags = r_.flags;
    const bool packed = flags & kPacked;
    if (!(flags & kCcbPre)) {           // preamble lives at the start of the source data
        r_.pre0 = rd32(mem_, src); src += 4;
        if (!packed) { r_.pre1 = rd32(mem_, src); src += 4; }
    }
    static const int kBpp[8] = {0, 1, 2, 4, 6, 8, 16, 0};
    const int bpp = kBpp[r_.pre0 & 7];
    if (!bpp) return;
    const bool uncoded = (r_.pre0 & kPre0Uncoded) && bpp >= 8;
    int rows = int((r_.pre0 >> 6) & 0x3FF) + 1;
    const int skipx = int((r_.pre0 >> 24) & 0xF);
    const bool lrform = !packed && bpp == 16 && (r_.pre1 & kPre1LrForm);
    if (lrform) rows *= 2;             // VCNT counts row pairs for LR-form source
    const int width = int(r_.pre1 & 0x7FF) + 1;
    const uint32_t stride = 4 * (bpp >= 8 ? ((r_.pre1 >> 16) & 0x3FF) + 2 : ((r_.pre1 >> 24) & 0xFF) + 2);

    // Corner engine state, kept in 12.20 fixed point
    int64_t ox = int64_t(r_.xpos) << 4, oy = int64_t(r_.ypos) << 4;
    int64_t hx = r_.hdx, hy = r_.hdy;
    const int64_t vx = int64_t(r_.vdx) << 4, vy = int64_t(r_.vdy) << 4;

    uint32_t row_addr = src;
    for (int row = 0; row < rows; ++row) {
        const int64_t nox = ox + vx, noy = oy + vy, nhx = hx + r_.hddx, nhy = hy + r_.hddy;
        auto emit = [&](int col, uint32_t raw) {
            Pixel px = decode(raw, bpp, uncoded);
            if (px.transparent) return;
            const int64_t c0 = col, c1 = col + 1;
            // Cel coordinates count from the bitmap's clip origin.
            const int32_t gx = t.clip_x * 65536, gy = t.clip_y * 65536;
            int32_t qx[4] = {gx + int32_t((ox + c0 * hx) >> 4), gx + int32_t((ox + c1 * hx) >> 4),
                             gx + int32_t((nox + c1 * nhx) >> 4), gx + int32_t((nox + c0 * nhx) >> 4)};
            int32_t qy[4] = {gy + int32_t((oy + c0 * hy) >> 4), gy + int32_t((oy + c1 * hy) >> 4),
                             gy + int32_t((noy + c1 * nhy) >> 4), gy + int32_t((noy + c0 * nhy) >> 4)};
            fill_quad(t, px, qx, qy);
        };
        if (packed) {
            const uint32_t hdr = rd32(mem_, row_addr);
            const uint32_t off = bpp >= 8 ? ((hdr >> 16) & 0x3FF) : (hdr >> 24);
            const uint32_t next = row_addr + (off + 2) * 4;
            uint64_t bit = uint64_t(row_addr) * 8 + (bpp >= 8 ? 16 : 8);
            const uint64_t end = uint64_t(next) * 8;
            int col = 0;
            while (bit + 2 <= end && col < 2048) {
                const uint32_t type = bits_at(mem_, bit, 2); bit += 2;
                if (type == 0) break;                                   // end of line
                const int count = int(bits_at(mem_, bit, 6)) + 1; bit += 6;
                if (type == 1) {                                        // literal pixels
                    for (int i = 0; i < count; ++i, bit += uint64_t(bpp)) emit(col++, bits_at(mem_, bit, bpp));
                } else if (type == 2) {                                 // transparent run
                    col += count;
                } else {                                                // one pixel repeated
                    const uint32_t raw = bits_at(mem_, bit, bpp); bit += uint64_t(bpp);
                    for (int i = 0; i < count; ++i) emit(col++, raw);
                }
            }
            row_addr = next;
        } else if (lrform) {
            const uint32_t base = src + uint32_t(row >> 1) * stride;
            for (int i = skipx; i < width; ++i)
                emit(i - skipx, rd16(mem_, base + uint32_t(i) * 4 + ((row & 1) ? 2 : 0)));
        } else {
            const uint64_t bit0 = uint64_t(row_addr) * 8;
            for (int i = skipx; i < width; ++i) emit(i - skipx, bits_at(mem_, bit0 + uint64_t(i) * uint64_t(bpp), bpp));
            row_addr += stride;
        }
        ox = nox; oy = noy; hx = nhx; hy = nhy;
    }
    // The corner engine leaves its position after the last row: the next cel
    // starts there unless its CCB loads a new one (YOXY).
    r_.xpos = int32_t(ox >> 4); r_.ypos = int32_t(oy >> 4);
}

int Gfx::draw_cels(const CelTarget& t, uint32_t ccb) {
    int drawn = 0;
    for (int guard = 0; ccb && ccb < kRamEnd - 23 && guard < 4096; ++guard) {
        // A CCB is read sequentially: optional groups are present only when
        // their load flag is set; values not loaded keep the previous ones.
        uint32_t a = ccb;
        r_.flags = rd32(mem_, a); a += 4;
        const uint32_t next_field = a;
        const uint32_t next = rd32(mem_, a); a += 4;
        const uint32_t src_field = a;
        uint32_t src = rd32(mem_, a); a += 4;
        // The PLUT pointer word is always fetched (and used only with LDPLUT);
        // the position is there only with YOXY. Without YOXY a cel starts where
        // the engine's position was left by the cel before it (see draw_one):
        // Escape from Monster Manor draws each line of text as one positioned
        // cel plus 12-byte cels (flags, next, source) for the following letters.
        const uint32_t plut_field = a;
        uint32_t plut = rd32(mem_, a); a += 4;
        if (r_.flags & kYoxy) { r_.xpos = int32_t(rd32(mem_, a)); r_.ypos = int32_t(rd32(mem_, a + 4)); a += 8; }
        if (r_.flags & kLdSize) {
            r_.hdx = int32_t(rd32(mem_, a)); r_.hdy = int32_t(rd32(mem_, a + 4));
            r_.vdx = int32_t(rd32(mem_, a + 8)); r_.vdy = int32_t(rd32(mem_, a + 12)); a += 16;
        }
        if (r_.flags & kLdPrs) { r_.hddx = int32_t(rd32(mem_, a)); r_.hddy = int32_t(rd32(mem_, a + 4)); a += 8; }
        if (r_.flags & kLdPpmp) { r_.pixc = rd32(mem_, a); a += 4; }
        if (r_.flags & kCcbPre) {
            r_.pre0 = rd32(mem_, a); a += 4;
            if (!(r_.flags & kPacked)) { r_.pre1 = rd32(mem_, a); a += 4; }
        }
        // Relative pointers count from the word after the pointer field.
        if (!(r_.flags & kSpAbs)) src = src_field + 4 + src;
        if (r_.flags & kLdPlut) {
            if (!(r_.flags & kPpAbs)) plut = plut_field + 4 + plut;
            static const int kBpp[8] = {0, 1, 2, 4, 6, 8, 16, 0};
            uint32_t pre0 = (r_.flags & kCcbPre) ? r_.pre0 : rd32(mem_, src);
            int bpp = kBpp[pre0 & 7];
            int n = (bpp >= 1 && bpp <= 4) ? (1 << bpp) : 32;
            for (int i = 0; i < n; ++i) r_.plut[i] = rd16(mem_, plut + uint32_t(i) * 2);
        }
        static const bool cel_trace = std::getenv("TDORT_CEL_TRACE") != nullptr;
        if (cel_trace)
            std::fprintf(stderr, "cel %08X fl=%08X x=%08X y=%08X hdx=%08X hdy=%08X vdx=%08X vdy=%08X hddx=%08X hddy=%08X pixc=%08X pre0=%08X pre1=%08X src=%08X\n",
                         ccb, r_.flags, uint32_t(r_.xpos), uint32_t(r_.ypos), uint32_t(r_.hdx), uint32_t(r_.hdy), uint32_t(r_.vdx), uint32_t(r_.vdy),
                         uint32_t(r_.hddx), uint32_t(r_.hddy), r_.pixc, r_.pre0, r_.pre1, src);
        if (!(r_.flags & kSkip)) { draw_one(t, src); ++drawn; }
        if (r_.flags & kLast) break;
        uint32_t nxt = (r_.flags & kNpAbs) ? next : next_field + 4 + next;
        if (!next && (r_.flags & kNpAbs)) break;
        ccb = nxt;
    }
    cels_drawn += uint64_t(drawn);
    return drawn;
}

} // namespace tdort
