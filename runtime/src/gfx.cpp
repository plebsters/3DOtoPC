// gfx.cpp - framebuffer scan-out and a software implementation of the 3DO
// CEL engine (data unpacker, pixel decoder, pixel processor, projector).
#include "tdort/gfx.h"
#include <algorithm>
#include <cstdlib>
#include <cstdio>
#include <cstring>

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

// One 16-bit frame buffer pixel of screen line y to XRGB, through the VDL
// colour table when there is one.
static inline uint32_t display_pixel(uint32_t pix, int y, const std::vector<uint32_t>& clut) {
    if (clut.size() >= size_t(Gfx::kHeight) * 32) {
        if (pix & 0x8000) return rgb555_to_xrgb(pix & 0x7FFF);   // bit 15: the colour table is bypassed
        if ((pix & 0x7FFF) == 0) return 0;                        // a 0 pixel shows the background colour (black), not entry 0
        const uint32_t* t = &clut[size_t(y) * 32];
        return (t[(pix >> 10) & 31] & 0xFF0000) | (t[(pix >> 5) & 31] & 0xFF00) | (t[pix & 31] & 0xFF);
    }
    return rgb555_to_xrgb(pix & 0x7FFF);
}

const uint32_t* Gfx::scan_out() {
    const int lines = std::clamp(display_lines_, 1, int(kHeight));
    const uint32_t bytes = kWidth * uint32_t(lines) * 2;
    // No valid display buffer yet: the last picture stays (and is enlarged
    // below, so out_frame() is always a full-size picture).
    if (display_addr_ == 0 || display_addr_ >= kRamEnd || display_addr_ > kRamEnd - bytes) { scale_up(-1, lines); return frame_.data(); }
    const uint8_t* src = &mem_.ram[display_addr_];
    std::fill(frame_.begin() + lines * kWidth, frame_.end(), 0u);
    for (int y = 0; y < lines; ++y) {
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
            frame_[y * kWidth + x] = display_pixel(pix, y, clut_);
        }
    }
    scale_up(lrform ? shadow_for(display_addr_, kWidth, lines, false) : -1, lines);
    return frame_.data();
}

// ---- hi-res shadows -----------------------------------------------------------

// The out_frame() picture at scale 2..4: the shadow k of the display buffer
// (-1: none), else the 1x picture enlarged.
void Gfx::scale_up(int k, int lines) {
    if (scale_ <= 1) return;
    const int s = scale_, W = kWidth * s;
    hi_frame_.resize(size_t(W) * kHeight * s);
    if (k >= 0) {
        Shadow& sh = shadows_[size_t(k)];
        sync(sh);
        const int hl = std::min(lines, sh.h) * s;
        const bool has_clut = clut_.size() >= size_t(kHeight) * 32;
        if (!has_clut && rgb_lut_.empty()) {
            rgb_lut_.resize(65536);
            for (uint32_t v = 0; v < 65536; ++v) rgb_lut_[v] = rgb555_to_xrgb(v & 0x7FFF);
        }
        for (int Y = 0; Y < hl; ++Y) {
            const uint16_t* row = &sh.hi[size_t(Y) * W];
            uint32_t* out = &hi_frame_[size_t(Y) * W];
            if (!has_clut) for (int X = 0; X < W; ++X) out[X] = rgb_lut_[row[X]];
            else for (int X = 0; X < W; ++X) out[X] = display_pixel(row[X], Y / s, clut_);
        }
        std::fill(hi_frame_.begin() + long(hl) * W, hi_frame_.end(), 0u);
    } else {                          // nothing drawn by cels: the 1x picture, enlarged
        for (int Y = 0; Y < kHeight * s; ++Y) {
            const uint32_t* in = &frame_[size_t(Y / s) * kWidth];
            uint32_t* out = &hi_frame_[size_t(Y) * W];
            for (int X = 0; X < W; ++X) out[X] = in[X / s];
        }
    }
}


void Gfx::set_scale(int s) {
    s = std::clamp(s, 1, 4);
    if (s == scale_) return;
    scale_ = s;
    drop_shadows();
    hi_frame_.clear();
}

int Gfx::shadow_for(uint32_t buffer, int w, int h, bool create) {
    for (size_t i = 0; i < shadows_.size(); ++i) {
        Shadow& sh = shadows_[i];
        if (sh.buffer == buffer && sh.w == w && sh.h >= h) { sh.used = ++use_clock_; return int(i); }
    }
    if (!create) return -1;
    // Screens only: small bitmaps are textures drawn into, gaining nothing.
    if (scale_ < 2 || !lrform || w < 256 || h < 200 || w > 1280 || h > 960) return -1;
    const uint32_t bytes = uint32_t(w) * uint32_t((h + 1) / 2) * 4;
    if (buffer == 0 || buffer >= kRamEnd || bytes > kRamEnd - buffer) return -1;
    // A bitmap overlapping a shadowed one replaces it (a game re-using memory).
    for (size_t i = 0; i < shadows_.size();) {
        const Shadow& o = shadows_[i];
        if (buffer < o.buffer + o.ref.size() && o.buffer < buffer + bytes) shadows_.erase(shadows_.begin() + long(i));
        else ++i;
    }
    constexpr size_t kMaxShadows = 6;
    if (shadows_.size() >= kMaxShadows) {
        size_t lru = 0;
        for (size_t i = 1; i < shadows_.size(); ++i) if (shadows_[i].used < shadows_[lru].used) lru = i;
        shadows_.erase(shadows_.begin() + long(lru));
    }
    Shadow sh;
    sh.buffer = buffer; sh.w = w; sh.h = h; sh.used = ++use_clock_;
    sh.hi.assign(size_t(w) * scale_ * size_t(h) * scale_, 0);
    sh.ref.assign(bytes, 0);
    // Every pixel differs from the reference: the first sync copies them all.
    for (uint32_t i = 0; i < bytes; ++i) sh.ref[i] = uint8_t(~mem_.ram[buffer + i]);
    shadows_.push_back(std::move(sh));
    return int(shadows_.size() - 1);
}

// Pixels the cel engine did not write since it last looked: copy them from
// the bitmap into the shadow, each as a scale x scale block. When much of the
// bitmap changed (the game cleared or redrew the screen) the whole shadow is
// taken from it, so edges drawn in the shadow alone do not linger on pixels
// the game happened to rewrite with the same value.
void Gfx::sync(Shadow& sh) {
    const uint8_t* m = &mem_.ram[sh.buffer];
    const size_t n = sh.ref.size();
    if (std::memcmp(m, sh.ref.data(), n) == 0) return;
    const size_t words = n / 4;
    size_t changed = 0;
    for (size_t k = 0; k < words; ++k) {
        uint32_t p, q;
        std::memcpy(&p, m + k * 4, 4); std::memcpy(&q, &sh.ref[k * 4], 4);
        changed += p != q;
    }
    const bool full = changed * 4 > words;
    const int s = scale_, W = sh.w * s;
    auto block = [&](int x, int y, uint16_t v) {
        if (y >= sh.h) return;
        for (int dy = 0; dy < s; ++dy) {
            uint16_t* d = &sh.hi[size_t(y * s + dy) * W + size_t(x) * s];
            for (int dx = 0; dx < s; ++dx) d[dx] = v;
        }
    };
    for (size_t k = 0; k < words; ++k) {
        const uint8_t* a = m + k * 4;
        const uint8_t* r = &sh.ref[k * 4];
        if (!full && std::memcmp(a, r, 4) == 0) continue;
        const int x = int(k % size_t(sh.w)), y = int(k / size_t(sh.w)) * 2;
        if (full || a[0] != r[0] || a[1] != r[1]) block(x, y, uint16_t((a[0] << 8) | a[1]));
        if (full || a[2] != r[2] || a[3] != r[3]) block(x, y + 1, uint16_t((a[2] << 8) | a[3]));
    }
    std::memcpy(sh.ref.data(), m, n);
}

void Gfx::note_write(uint32_t addr, uint32_t len) {
    for (Shadow& sh : shadows_) {
        const uint64_t a = std::max<uint64_t>(addr, sh.buffer);
        const uint64_t b = std::min<uint64_t>(uint64_t(addr) + len, uint64_t(sh.buffer) + sh.ref.size());
        for (uint64_t i = a; i < b; ++i) sh.ref[i - sh.buffer] = uint8_t(~mem_.ram[i]);
    }
}

// The same quad as fill_quad, on the shadow's finer grid.
void Gfx::fill_quad_hi(const CelTarget& t, const Pixel& px, const int32_t (&x)[4], const int32_t (&y)[4]) {
    Shadow& sh = shadows_[size_t(cur_)];
    const int s = scale_, W = sh.w * s, H = sh.h * s;
    const int cx0 = std::max(0, t.clip_x * s), cy0 = std::max(0, t.clip_y * s);
    const int cx1 = std::min(W, (t.clip_x + t.clip_w) * s), cy1 = std::min(H, (t.clip_y + t.clip_h) * s);
    if (cx0 >= cx1 || cy0 >= cy1) return;
    int64_t X[4], Y[4];
    for (int i = 0; i < 4; ++i) { X[i] = int64_t(x[i]) * s; Y[i] = int64_t(y[i]) * s; }
    const uint32_t ctl = px.pmode ? (r_.pixc >> 16) & 0xFFFF : r_.pixc & 0xFFFF;
    const bool needs_fb = (ctl & 0x8000) || ((ctl >> 6) & 3) == 2;
    const uint16_t flat = needs_fb ? 0 : process(px, 0);
    uint16_t* hi = sh.hi.data();
    auto put = [&](int xx, int yy) {
        uint16_t& d = hi[size_t(yy) * W + size_t(xx)];
        d = needs_fb ? process(px, d) : flat;
    };
    if (Y[0] == Y[1] && Y[2] == Y[3] && X[0] == X[3] && X[1] == X[2]) {
        const int64_t xa = std::min(X[0], X[1]), xb = std::max(X[0], X[1]);
        const int64_t ya = std::min(Y[0], Y[3]), yb = std::max(Y[0], Y[3]);
        const int px0 = int(std::clamp<int64_t>((xa + 0x7FFF) >> 16, cx0, cx1)), px1 = int(std::clamp<int64_t>((xb + 0x7FFF) >> 16, cx0, cx1));
        const int py0 = int(std::clamp<int64_t>((ya + 0x7FFF) >> 16, cy0, cy1)), py1 = int(std::clamp<int64_t>((yb + 0x7FFF) >> 16, cy0, cy1));
        for (int yy = py0; yy < py1; ++yy)
            for (int xx = px0; xx < px1; ++xx) put(xx, yy);
        return;
    }
    const int64_t area = (X[1] - X[0]) * (Y[2] - Y[0]) - (Y[1] - Y[0]) * (X[2] - X[0]);
    const int sign = area > 0 ? 1 : -1;
    const int64_t minx = std::min({X[0], X[1], X[2], X[3]}), maxx = std::max({X[0], X[1], X[2], X[3]});
    const int64_t miny = std::min({Y[0], Y[1], Y[2], Y[3]}), maxy = std::max({Y[0], Y[1], Y[2], Y[3]});
    const int bx0 = int(std::clamp<int64_t>(minx >> 16, cx0, cx1)), bx1 = int(std::clamp<int64_t>((maxx >> 16) + 1, cx0, cx1));
    const int by0 = int(std::clamp<int64_t>(miny >> 16, cy0, cy1)), by1 = int(std::clamp<int64_t>((maxy >> 16) + 1, cy0, cy1));
    // Edge functions, stepped per pixel instead of recomputed: e(x, y) >= 0
    // on the inside of every edge (multiplied by the winding's sign).
    int64_t ex[4], ey[4], row_e[4], dx_e[4], dy_e[4];
    const int64_t sx0 = (int64_t(bx0) << 16) + 0x8000, cy0f = (int64_t(by0) << 16) + 0x8000;
    for (int e = 0; e < 4; ++e) {
        const int f = (e + 1) & 3;
        ex[e] = X[f] - X[e]; ey[e] = Y[f] - Y[e];
        row_e[e] = (ex[e] * (cy0f - Y[e]) - ey[e] * (sx0 - X[e])) * sign;
        dx_e[e] = -ey[e] * 65536 * sign;
        dy_e[e] = ex[e] * 65536 * sign;
    }
    const int bw = bx1 - bx0;
    for (int yy = by0; yy < by1; ++yy) {
        if (bw <= 6) {                    // narrow: test each pixel
            int64_t e0 = row_e[0], e1 = row_e[1], e2 = row_e[2], e3 = row_e[3];
            for (int xx = bx0; xx < bx1; ++xx) {
                if ((e0 | e1 | e2 | e3) >= 0) put(xx, yy);
                e0 += dx_e[0]; e1 += dx_e[1]; e2 += dx_e[2]; e3 += dx_e[3];
            }
        } else {                          // wide: the run where all four edges agree
            int64_t lo = 0, hi_k = bw - 1;
            for (int e = 0; e < 4 && lo <= hi_k; ++e) {
                const int64_t r = row_e[e], d = dx_e[e];
                if (d > 0) {              // r + k*d >= 0  <=>  k >= ceil(-r / d)
                    const int64_t k = r >= 0 ? -(r / d) : (-r + d - 1) / d;
                    lo = std::max(lo, k);
                } else if (d < 0) {       // k <= floor(r / -d)
                    const int64_t D = -d;
                    const int64_t k = r >= 0 ? r / D : -((-r + D - 1) / D);
                    hi_k = std::min(hi_k, k);
                } else if (r < 0) {
                    hi_k = -1;
                }
            }
            for (int64_t k = lo; k <= hi_k; ++k) put(bx0 + int(k), yy);
        }
        for (int e = 0; e < 4; ++e) row_e[e] += dy_e[e];
    }
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
    if (cur_ >= 0) {
        Shadow& sh = shadows_[size_t(cur_)];
        const uint32_t o = a - sh.buffer;
        if (a >= sh.buffer && o + 1 < sh.ref.size()) { sh.ref[o] = uint8_t(v >> 8); sh.ref[o + 1] = uint8_t(v); }
    }
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
    // The pixel's top bit as decoded (before POVER) goes to the frame buffer's
    // bit 15, which the display reads: with a VDL colour table, such pixels
    // show their RGB directly (Wing Commander III's movies mark the pixels
    // that are not in the scene's table this way).
    p.msb = p.pmode != 0;
    // P-mode override from the CCB (POVER)
    switch ((flags >> 7) & 3) {
    case 2: p.pmode = 0; break;
    case 3: p.pmode = 1; break;
    default: break;
    }
    // Only the CCB's BGND flag keeps 000 pixels opaque: the cel engine does not
    // test the preamble's BGND bit (Opera, the reference emulator, never does).
    // Return Fire sets that bit on its bushes and tree shadows, which then
    // drew their empty pixels as black boxes. TDORT_PRE0_BGND=1: honour it.
    static const bool pre0_bgnd = std::getenv("TDORT_PRE0_BGND") != nullptr;
    p.transparent = p.color == 0 && !(pre0_bgnd && (r_.pre0 & kPre0Bgnd)) && !(flags & kBgnd);
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
    static const bool old_msb = std::getenv("TDORT_OLD_FB_MSB") != nullptr;
    if (p.msb && !old_msb) out |= 0x8000;
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
    if (cur_ >= 0) fill_quad_hi(t, px, x, y);

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
    cur_ = scale_ > 1 ? shadow_for(t.buffer, t.width, t.height, true) : -1;
    if (cur_ >= 0) sync(shadows_[size_t(cur_)]);
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
    cur_ = -1;
    return drawn;
}

} // namespace tdort
