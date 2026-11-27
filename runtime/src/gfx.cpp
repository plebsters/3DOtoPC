// gfx.cpp - framebuffer scan-out and a software implementation of the 3DO
// CEL engine (data unpacker, pixel decoder, pixel processor, projector).
#include "tdort/gfx.h"
#include <algorithm>
#include <cmath>
#include <cstdlib>
#include <cstdio>
#include <cstring>
#include <atomic>
#include <chrono>
#include <condition_variable>
#include <deque>
#include <mutex>
#include <thread>

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
    // Up to 24 bits from a 4-byte big-endian window (cel pixels are at most
    // 16 bits); bit by bit only at the very end of memory.
    const uint64_t first = bitpos >> 3;
    if (n > 0 && n <= 24 && first + 4 <= kRamEnd) {
        const uint8_t* q = &m.ram[size_t(first)];
        const uint32_t w = (uint32_t(q[0]) << 24) | (uint32_t(q[1]) << 16) | (uint32_t(q[2]) << 8) | q[3];
        return (w << (bitpos & 7)) >> (32 - n);
    }
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

// One row of a cel, as the corner engine walked it: each pixel's corners are
// affine in its column, so the row is stored once and its pixels as 8 bytes
// each; the workers rebuild every pixel's quad exactly as fill_quad saw it.
struct HiRow {
    int64_t ox, oy, hx, hy, nox, noy, nhx, nhy;   // corner engine, 12.20
    int32_t gx, gy;                       // clip origin, 16.16
    int16_t clip_x, clip_y, clip_w, clip_h;
    uint32_t pixc, flags;
    uint32_t first, count;                // pixels in HiPool::pix
    uint8_t scale;
};
struct HiPix { uint16_t col, color; uint8_t bits, amv_r, amv_g, amv_b; };   // bits: 1 pmode, 2 msb

// ---- hi-res shadows -----------------------------------------------------------

// The out_frame() picture at scale 2..4: the shadow k of the display buffer
// (-1: none), else the 1x picture enlarged.
void Gfx::scale_up(int k, int lines) {
    if (scale_ <= 1) return;
    hi_wait();
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
        auto rows = [&](int y0, int y1) {
            for (int Y = y0; Y < y1; ++Y) {
                const uint16_t* row = &sh.hi[size_t(Y) * W];
                uint32_t* out = &hi_frame_[size_t(Y) * W];
                if (!has_clut) for (int X = 0; X < W; ++X) out[X] = rgb_lut_[row[X]];
                else for (int X = 0; X < W; ++X) out[X] = display_pixel(row[X], Y / s, clut_);
            }
        };
        // Split over a few threads when the picture is big (3x, 4x).
        const int nt = size_t(W) * size_t(hl) >= 600000 ? std::clamp(int(std::thread::hardware_concurrency()), 1, 4) : 1;
        if (nt <= 1) rows(0, hl);
        else {
            std::vector<std::thread> helpers;
            for (int i = 1; i < nt; ++i) helpers.emplace_back(rows, hl * i / nt, hl * (i + 1) / nt);
            rows(0, hl / nt);
            for (auto& t : helpers) t.join();
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
    hi_wait();                            // shadows may move or go below
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
    hi_wait();                            // queued drawing lands first
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

// ---- drawing the shadows on worker threads --------------------------------
// Each quad for a shadow becomes a command; commands are handed to worker
// threads in batches (at the end of each DrawCels), and each worker draws the
// rows of every batch that belong to it (stripes of 8 game lines, dealt out in turn),
// in order. So the finer picture is drawn on other cores while the game runs
// on, and the result is the same as drawing it here. The game's thread waits
// for them only before it reads or changes a shadow (scan-out, copying pixels
// the game wrote, dropping shadows). TDORT_HI_THREADS=n: n workers (0 = draw
// on the game's thread).

struct HiCmd {
    int32_t x[4], y[4];                   // corners, 16.16 on the 1x grid
    int16_t clip_x, clip_y, clip_w, clip_h;   // the bitmap's clip window (1x)
    uint8_t scale;
    Gfx::PixelState px;
    uint32_t pixc, flags;
};


static void hi_raster(const HiCmd& c, uint16_t* hi, int W, int H, int w, int T) {
    const int s = c.scale;
    const int cx0 = std::max(0, c.clip_x * s), cy0 = std::max(0, c.clip_y * s);
    const int cx1 = std::min(W, (c.clip_x + c.clip_w) * s), cy1 = std::min(H, (c.clip_y + c.clip_h) * s);
    if (cx0 >= cx1 || cy0 >= cy1) return;
    int64_t X[4], Y[4];
    for (int i = 0; i < 4; ++i) { X[i] = int64_t(c.x[i]) * s; Y[i] = int64_t(c.y[i]) * s; }
    const int64_t miny = std::min({Y[0], Y[1], Y[2], Y[3]}), maxy = std::max({Y[0], Y[1], Y[2], Y[3]});
    const int ymin = int(std::clamp<int64_t>(miny >> 16, cy0, cy1)), ymax = int(std::clamp<int64_t>((maxy >> 16) + 1, cy0, cy1));
    if (ymin >= ymax) return;
    if (T > 1) {                          // none of this worker's stripes in [ymin, ymax)?
        bool any = false;
        for (int yy = ymin; yy < ymax && !any; yy = (yy | 7) + 1) any = ((yy >> 3) % T) == w;
        if (!any) return;
    }
    const uint32_t ctl = c.px.pmode ? (c.pixc >> 16) & 0xFFFF : c.pixc & 0xFFFF;
    const bool needs_fb = (ctl & 0x8000) || ((ctl >> 6) & 3) == 2;
    const uint16_t flat = needs_fb ? 0 : Gfx::process_state(c.px, 0, c.pixc, c.flags);
    auto row_ok = [&](int yy) { return T <= 1 || ((yy >> 3) % T) == w; };
    auto put = [&](int xx, int yy) {
        uint16_t& d = hi[size_t(yy) * W + size_t(xx)];
        d = needs_fb ? Gfx::process_state(c.px, d, c.pixc, c.flags) : flat;
    };
    if (Y[0] == Y[1] && Y[2] == Y[3] && X[0] == X[3] && X[1] == X[2]) {
        const int64_t xa = std::min(X[0], X[1]), xb = std::max(X[0], X[1]);
        const int64_t ya = std::min(Y[0], Y[3]), yb = std::max(Y[0], Y[3]);
        const int px0 = int(std::clamp<int64_t>((xa + 0x7FFF) >> 16, cx0, cx1)), px1 = int(std::clamp<int64_t>((xb + 0x7FFF) >> 16, cx0, cx1));
        const int py0 = int(std::clamp<int64_t>((ya + 0x7FFF) >> 16, cy0, cy1)), py1 = int(std::clamp<int64_t>((yb + 0x7FFF) >> 16, cy0, cy1));
        for (int yy = py0; yy < py1; ++yy)
            if (row_ok(yy)) for (int xx = px0; xx < px1; ++xx) put(xx, yy);
        return;
    }
    const int64_t area = (X[1] - X[0]) * (Y[2] - Y[0]) - (Y[1] - Y[0]) * (X[2] - X[0]);
    const int sign = area > 0 ? 1 : -1;
    const int64_t minx = std::min({X[0], X[1], X[2], X[3]}), maxx = std::max({X[0], X[1], X[2], X[3]});
    const int bx0 = int(std::clamp<int64_t>(minx >> 16, cx0, cx1)), bx1 = int(std::clamp<int64_t>((maxx >> 16) + 1, cx0, cx1));
    const int by0 = ymin, by1 = ymax;
    // Edge functions, stepped per pixel instead of recomputed: e(x, y) >= 0
    // on the inside of every edge (multiplied by the winding's sign).
    int64_t row_e[4], dx_e[4], dy_e[4];
    const int64_t sx0 = (int64_t(bx0) << 16) + 0x8000, cy0f = (int64_t(by0) << 16) + 0x8000;
    for (int e = 0; e < 4; ++e) {
        const int f = (e + 1) & 3;
        const int64_t ex = X[f] - X[e], ey = Y[f] - Y[e];
        row_e[e] = (ex * (cy0f - Y[e]) - ey * (sx0 - X[e])) * sign;
        dx_e[e] = -ey * 65536 * sign;
        dy_e[e] = ex * 65536 * sign;
    }
    const int bw = bx1 - bx0;
    for (int yy = by0; yy < by1; ++yy) {
        if (row_ok(yy)) {
            if (bw <= 6) {                // narrow: test each pixel
                int64_t e0 = row_e[0], e1 = row_e[1], e2 = row_e[2], e3 = row_e[3];
                for (int xx = bx0; xx < bx1; ++xx) {
                    if ((e0 | e1 | e2 | e3) >= 0) put(xx, yy);
                    e0 += dx_e[0]; e1 += dx_e[1]; e2 += dx_e[2]; e3 += dx_e[3];
                }
            } else {                      // wide: the run where all four edges agree
                int64_t lo = 0, hi_k = bw - 1;
                for (int e = 0; e < 4 && lo <= hi_k; ++e) {
                    const int64_t r = row_e[e], d = dx_e[e];
                    if (d > 0) {          // r + k*d >= 0  <=>  k >= ceil(-r / d)
                        const int64_t k = r >= 0 ? -(r / d) : (-r + d - 1) / d;
                        lo = std::max(lo, k);
                    } else if (d < 0) {   // k <= floor(r / -d)
                        const int64_t D = -d;
                        const int64_t k = r >= 0 ? r / D : -((-r + D - 1) / D);
                        hi_k = std::min(hi_k, k);
                    } else if (r < 0) {
                        hi_k = -1;
                    }
                }
                for (int64_t k = lo; k <= hi_k; ++k) put(bx0 + int(k), yy);
            }
        }
        for (int e = 0; e < 4; ++e) row_e[e] += dy_e[e];
    }
}

// The pixel processor set up for one cel row and P-mode: the control
// half-word's table (or normal mode, where the result is the cel colour).
// A pixel's result for a frame buffer value is then three table reads.
struct RowBlend {
    const uint8_t* t = nullptr;
    bool direct = false, fix_black = false, slow = false, old_msb = false;
};
static void make_row_blend(RowBlend& b, uint32_t ctl, uint32_t flags);
static inline uint16_t row_blend(const RowBlend& b, const Gfx::PixelState& ps, uint16_t fb, uint32_t pixc, uint32_t flags) {
    if (b.slow) return Gfx::process_state(ps, fb, pixc, flags);
    uint16_t out;
    if (b.direct) out = ps.color;
    else {
        const uint8_t* t = b.t;
        out = uint16_t((uint32_t(t[(uint32_t(ps.amv_r < 8 ? ps.amv_r : 8) * 32 + ((ps.color >> 10) & 31)) * 32 + ((fb >> 10) & 31)]) << 10)
                     | (uint32_t(t[(uint32_t(ps.amv_g < 8 ? ps.amv_g : 8) * 32 + ((ps.color >> 5) & 31)) * 32 + ((fb >> 5) & 31)]) << 5)
                     | t[(uint32_t(ps.amv_b < 8 ? ps.amv_b : 8) * 32 + (ps.color & 31)) * 32 + (fb & 31)]);
    }
    if (out == 0 && b.fix_black) out = 0x0400;
    if (ps.msb && !b.old_msb) out |= 0x8000;
    return out;
}
static inline int64_t min4(const int64_t* v) { int64_t a = v[0] < v[1] ? v[0] : v[1], b = v[2] < v[3] ? v[2] : v[3]; return a < b ? a : b; }
static inline int64_t max4(const int64_t* v) { int64_t a = v[0] > v[1] ? v[0] : v[1], b = v[2] > v[3] ? v[2] : v[3]; return a > b ? a : b; }

// The stripe height for a batch: workers take turns by stripes of rows. Big
// pixel quads (magnified textures) want tall stripes (8 game lines, 32 rows at
// 4x), so few quads straddle two workers and get set up twice; tiny ones (far
// textures, many texels to a pixel) rarely straddle, and short 8-row stripes
// share a crowded band of the screen among more workers. Judged from a sample
// of the batch's rows: the height of a pixel's quad, in shadow rows.
static int hi_stripe(const HiRow* rows, size_t n, int scale) {
    if (!n || scale <= 1) return 8;
    double sum = 0; int cnt = 0;
    const size_t step = std::max<size_t>(1, n / 64);
    for (size_t i = 0; i < n; i += step, ++cnt) {
        const HiRow& r = rows[i];
        const double down = std::fabs(double(r.noy - r.oy)), across = std::fabs(double(r.hy));
        sum += (down + across) * scale / 1048576.0;
    }
    return sum / cnt >= 1.0 ? 8 * scale : 8;
}

// Draws one recorded cel row into a shadow: every pixel's quad, scaled,
// filled where pixel centres are inside all four edges (as fill_quad does at
// 1x). Worker w of T draws only its own stripes. Everything that is the
// same for the whole row (clip window, pixel processor modes) is worked out
// once per row.
static void hi_raster_row(const HiRow& r, const HiPix* pix, uint16_t* hi, int W, int H, int w, int T, int stripe) {
    if (!r.count) return;
    const int s = r.scale;
    // Workers take turns by stripes of `stripe` rows (see hi_stripe).
    // Per row of the shadow: which worker owns it, and where the next stripe starts.
    thread_local std::vector<uint8_t> owner; thread_local std::vector<int32_t> next_start;
    thread_local int t_H = -1, t_stripe = -1, t_T = -1;
    if (T > 1 && (t_H != H || t_stripe != stripe || t_T != T)) {
        owner.resize(size_t(H) + 1); next_start.resize(size_t(H) + 1);
        for (int y = 0; y <= H; ++y) { owner[size_t(y)] = uint8_t((y / stripe) % T); next_start[size_t(y)] = (y / stripe + 1) * stripe; }
        t_H = H; t_stripe = stripe; t_T = T;
    }
    // Rows this cel row can touch: its outer corners bound every pixel's quad.
    if (T > 1) {
        const int64_t c0 = pix[0].col, c1 = int64_t(pix[r.count - 1].col) + 1;
        const int64_t ys[4] = {r.oy + c0 * r.hy, r.oy + c1 * r.hy, r.noy + c0 * r.nhy, r.noy + c1 * r.nhy};
        const int64_t lo = (int64_t(r.gy) + (std::min({ys[0], ys[1], ys[2], ys[3]}) >> 4)) * s;
        const int64_t hi_ = (int64_t(r.gy) + (std::max({ys[0], ys[1], ys[2], ys[3]}) >> 4)) * s;
        const int64_t y0 = std::max<int64_t>(0, (lo >> 16) - 1), y1 = std::min<int64_t>(H, (hi_ >> 16) + 2);
        bool any = false;
        for (int64_t yy = y0; yy < y1 && !any; yy = next_start[size_t(yy)]) any = owner[size_t(yy)] == w;
        if (!any) return;
    }
    const int cx0 = std::max(0, r.clip_x * s), cy0 = std::max(0, r.clip_y * s);
    const int cx1 = std::min(W, (r.clip_x + r.clip_w) * s), cy1 = std::min(H, (r.clip_y + r.clip_h) * s);
    if (cx0 >= cx1 || cy0 >= cy1) return;
    const bool acw = r.flags & 0x00040000u, accw = r.flags & 0x00020000u;
    if (!acw && !accw) return;
    bool needs_fb[2];
    RowBlend rb[2];
    for (int m = 0; m < 2; ++m) {
        const uint32_t ctl = m ? (r.pixc >> 16) & 0xFFFF : r.pixc & 0xFFFF;
        needs_fb[m] = (ctl & 0x8000) || ((ctl >> 6) & 3) == 2;
        make_row_blend(rb[m], ctl, r.flags);
    }
    // Rows (of the shadow) per column when every corner moves the same way;
    // rate is the fastest such movement (rows per column, signed).
    const bool mono = T > 1 && ((r.hy > 0 && r.nhy > 0) || (r.hy < 0 && r.nhy < 0));
    const double rate = !mono ? 0.0 : (r.hy > 0 ? double(std::max(r.hy, r.nhy)) : double(std::min(r.hy, r.nhy))) * s / 1048576.0;
    const int64_t cx0f = int64_t(cx0) << 16, cx1f = int64_t(cx1) << 16, cy0f_ = int64_t(cy0) << 16, cy1f = int64_t(cy1) << 16;
    for (uint32_t i = 0; i < r.count; ++i) {
        const HiPix& p = pix[i];
        const int64_t c0 = p.col, c1 = int64_t(p.col) + 1;
        // The rows first: a quad in another worker's stripes is dropped
        // before the rest is worked out.
        int32_t qx[4], qy[4];
        qy[0] = r.gy + int32_t((r.oy + c0 * r.hy) >> 4);   qy[1] = r.gy + int32_t((r.oy + c1 * r.hy) >> 4);
        qy[2] = r.gy + int32_t((r.noy + c1 * r.nhy) >> 4); qy[3] = r.gy + int32_t((r.noy + c0 * r.nhy) >> 4);
        int64_t X[4], Y[4];
        for (int k = 0; k < 4; ++k) Y[k] = int64_t(qy[k]) * s;
        const int64_t miny = min4(Y), maxy = max4(Y);
        if (maxy < cy0f_ - 65536 || miny >= cy1f) continue;
        const int ymin = int(std::clamp<int64_t>(miny >> 16, cy0, cy1)), ymax = int(std::clamp<int64_t>((maxy >> 16) + 1, cy0, cy1));
        if (ymin >= ymax) continue;
        if (T > 1) {                      // none of this worker's stripes in [ymin, ymax)?
            bool any = false;
            for (int yy = ymin; yy < ymax && !any; yy = next_start[size_t(yy)]) any = owner[size_t(yy)] == w;
            if (!any) {
                // A row that runs steadily down (or up) the screen, like a wall
                // drawn as tall columns, crosses every stripe: skip the columns
                // that can't reach this worker's next stripe instead of
                // working each one out. The bound is conservative (2 rows of
                // margin), so only quads that draw nothing here are skipped.
                if (mono) {
                    double k = 0;
                    if (rate > 0) {       // moving down: the next row of ours below
                        int y = ymax;
                        while (y < H && owner[size_t(y)] != w) y = next_start[size_t(y)];
                        if (y >= H) break;   // nothing of ours further down this row
                        k = (double(y) - 2.0 - double(maxy) / 65536.0) / rate;
                    } else {              // moving up: the last row of ours above
                        int y = ymin - 1;
                        while (y >= 0 && owner[size_t(y)] != w) y = (y / stripe) * stripe - 1;
                        if (y < 0) break;
                        k = (double(miny) / 65536.0 - double(y + 1) - 2.0) / -rate;
                    }
                    if (k >= 2.0) {
                        const int64_t target = int64_t(p.col) + int64_t(k);
                        while (i + 1 < r.count && int64_t(pix[i + 1].col) < target) ++i;
                    }
                }
                continue;
            }
        }
        qx[0] = r.gx + int32_t((r.ox + c0 * r.hx) >> 4);   qx[1] = r.gx + int32_t((r.ox + c1 * r.hx) >> 4);
        qx[2] = r.gx + int32_t((r.nox + c1 * r.nhx) >> 4); qx[3] = r.gx + int32_t((r.nox + c0 * r.nhx) >> 4);
        // fill_quad's face culling
        const int64_t area1 = int64_t(qx[1] - qx[0]) * (qy[2] - qy[0]) - int64_t(qy[1] - qy[0]) * (qx[2] - qx[0]);
        if (area1 == 0 || (area1 > 0 && !acw) || (area1 < 0 && !accw)) continue;
        for (int k = 0; k < 4; ++k) X[k] = int64_t(qx[k]) * s;
        const int64_t minx = min4(X), maxx = max4(X);
        // Entirely outside the clip window: nothing to draw (the bounds below would be empty).
        if (maxx < cx0f - 65536 || minx >= cx1f) continue;
        // No pixel centre (n + 0.5) inside the bounding box across or down: a
        // quad smaller than a pixel that falls between centres draws nothing
        // (far walls shrink many texels into each pixel).
        if (((maxy - 0x8000) >> 16) < ((miny - 0x8000 + 0xFFFF) >> 16) || ((maxx - 0x8000) >> 16) < ((minx - 0x8000 + 0xFFFF) >> 16)) continue;
        const int pm = p.bits & 1;
        Gfx::PixelState ps{p.color, uint8_t(pm), p.amv_r, p.amv_g, p.amv_b, (p.bits & 2) != 0};
        const bool nfb = needs_fb[pm];
        const RowBlend& b = rb[pm];
        const uint16_t flat = nfb ? 0 : row_blend(b, ps, 0, r.pixc, r.flags);
        auto qb = [&](uint16_t fb) { return row_blend(b, ps, fb, r.pixc, r.flags); };
        auto row_ok = [&](int yy) { return T <= 1 || owner[size_t(yy)] == w; };
        if (Y[0] == Y[1] && Y[2] == Y[3] && X[0] == X[3] && X[1] == X[2]) {
            const int64_t xa = std::min(X[0], X[1]), xb = std::max(X[0], X[1]);
            const int64_t ya = std::min(Y[0], Y[3]), yb = std::max(Y[0], Y[3]);
            const int px0 = int(std::clamp<int64_t>((xa + 0x7FFF) >> 16, cx0, cx1)), px1 = int(std::clamp<int64_t>((xb + 0x7FFF) >> 16, cx0, cx1));
            const int py0 = int(std::clamp<int64_t>((ya + 0x7FFF) >> 16, cy0, cy1)), py1 = int(std::clamp<int64_t>((yb + 0x7FFF) >> 16, cy0, cy1));
            for (int yy = py0; yy < py1; ++yy) {
                if (!row_ok(yy)) continue;
                uint16_t* d = hi + size_t(yy) * W;
                if (nfb) for (int xx = px0; xx < px1; ++xx) d[xx] = qb(d[xx]);
                else for (int xx = px0; xx < px1; ++xx) d[xx] = flat;
            }
            continue;
        }
        const int64_t area = (X[1] - X[0]) * (Y[2] - Y[0]) - (Y[1] - Y[0]) * (X[2] - X[0]);
        const int sign = area > 0 ? 1 : -1;
        const int bx0 = int(std::clamp<int64_t>(minx >> 16, cx0, cx1)), bx1 = int(std::clamp<int64_t>((maxx >> 16) + 1, cx0, cx1));
        int64_t row_e[4], dx_e[4], dy_e[4];
        const int64_t sx0 = (int64_t(bx0) << 16) + 0x8000, cy0f = (int64_t(ymin) << 16) + 0x8000;
        for (int e = 0; e < 4; ++e) {
            const int f = (e + 1) & 3;
            const int64_t ex = X[f] - X[e], ey = Y[f] - Y[e];
            row_e[e] = (ex * (cy0f - Y[e]) - ey * (sx0 - X[e])) * sign;
            dx_e[e] = -ey * 65536 * sign;
            dy_e[e] = ex * 65536 * sign;
        }
        const int bw = bx1 - bx0;
        for (int yy = ymin; yy < ymax; ++yy) {
            if (row_ok(yy)) {
                uint16_t* d = hi + size_t(yy) * W;
                int64_t lo = 0, hi_k = bw - 1;
                if (bw <= 6) {            // narrow: test each pixel
                    int64_t e0 = row_e[0], e1 = row_e[1], e2 = row_e[2], e3 = row_e[3];
                    for (int xx = bx0; xx < bx1; ++xx) {
                        if ((e0 | e1 | e2 | e3) >= 0) d[xx] = nfb ? qb(d[xx]) : flat;
                        e0 += dx_e[0]; e1 += dx_e[1]; e2 += dx_e[2]; e3 += dx_e[3];
                    }
                } else {                  // wide: the run where all four edges agree
                    for (int e = 0; e < 4 && lo <= hi_k; ++e) {
                        const int64_t rr = row_e[e], dd = dx_e[e];
                        if (dd > 0) {     // rr + k*dd >= 0  <=>  k >= ceil(-rr / dd)
                            const int64_t k = rr >= 0 ? -(rr / dd) : (-rr + dd - 1) / dd;
                            lo = std::max(lo, k);
                        } else if (dd < 0) {   // k <= floor(rr / -dd)
                            const int64_t D = -dd;
                            const int64_t k = rr >= 0 ? rr / D : -((-rr + D - 1) / D);
                            hi_k = std::min(hi_k, k);
                        } else if (rr < 0) {
                            hi_k = -1;
                        }
                    }
                    if (nfb) for (int64_t k = lo; k <= hi_k; ++k) { uint16_t& v = d[bx0 + int(k)]; v = qb(v); }
                    else for (int64_t k = lo; k <= hi_k; ++k) d[bx0 + int(k)] = flat;
                }
            }
            for (int e = 0; e < 4; ++e) row_e[e] += dy_e[e];
        }
    }
}

struct Gfx::HiPool {
    struct Batch { std::vector<HiRow> rows; std::vector<HiPix> pix; uint16_t* hi; int W, H; int left; int stripe; };
    std::vector<std::thread> th;
    std::mutex m;
    std::condition_variable work_cv, done_cv;
    std::deque<std::shared_ptr<Batch>> q;
    std::vector<size_t> next;             // per worker: the next batch number it draws
    size_t base = 0;                      // number of q.front()
    bool quit = false;
    int T = 0;
    std::vector<HiRow> rows;              // cel rows not handed out yet (one shadow)
    std::vector<HiPix> pix;
    uint16_t* p_hi = nullptr; int p_W = 0, p_H = 0;

    HiPool() {
        int n = std::clamp(int(std::thread::hardware_concurrency()) - 1, 0, 6);
        if (const char* e = std::getenv("TDORT_HI_THREADS")) n = std::clamp(std::atoi(e), 0, 16);
        T = n;
        next.assign(size_t(n), 0);
        for (int w = 0; w < n; ++w) th.emplace_back([this, w] { run(w); });
    }
    ~HiPool() {
        { std::lock_guard<std::mutex> l(m); quit = true; }
        work_cv.notify_all();
        for (auto& t : th) t.join();
    }
    void run(int w) {
        for (;;) {
            std::shared_ptr<Batch> b;
            {
                std::unique_lock<std::mutex> l(m);
                work_cv.wait(l, [&] { return quit || next[size_t(w)] < base + q.size(); });
                if (quit && next[size_t(w)] >= base + q.size()) return;
                b = q[next[size_t(w)] - base];
            }
            for (const HiRow& r : b->rows) hi_raster_row(r, b->pix.data() + r.first, b->hi, b->W, b->H, w, T, b->stripe);
            {
                std::lock_guard<std::mutex> l(m);
                ++next[size_t(w)];
                --b->left;
                while (!q.empty() && q.front()->left == 0) { q.pop_front(); ++base; }
            }
            done_cv.notify_all();
        }
    }
    void target(uint16_t* hi, int W, int H) {
        if (hi != p_hi || W != p_W || H != p_H) { submit(); p_hi = hi; p_W = W; p_H = H; }
    }
    void submit() {
        if (rows.empty()) { pix.clear(); return; }
        // TDORT_HI_DUMP=file: append the batches drawn without workers (benchmarking).
        static FILE* dump = [] { const char* e = std::getenv("TDORT_HI_DUMP"); return e ? std::fopen(e, "wb") : nullptr; }();
        if (dump && T == 0) {
            const uint32_t hdr[4] = {uint32_t(p_W), uint32_t(p_H), uint32_t(rows.size()), uint32_t(pix.size())};
            std::fwrite(hdr, 4, 4, dump);
            std::fwrite(rows.data(), sizeof(HiRow), rows.size(), dump);
            std::fwrite(pix.data(), sizeof(HiPix), pix.size(), dump);
            std::fflush(dump);
        }
        if (T == 0) {                     // no workers: draw now
            for (const HiRow& r : rows) hi_raster_row(r, pix.data() + r.first, p_hi, p_W, p_H, 0, 1, 8);
            rows.clear(); pix.clear();
            return;
        }
        auto b = std::make_shared<Batch>();
        b->rows.swap(rows); b->pix.swap(pix);
        b->hi = p_hi; b->W = p_W; b->H = p_H; b->left = T;
        b->stripe = hi_stripe(b->rows.data(), b->rows.size(), b->rows.empty() ? 1 : b->rows[0].scale);
        rows.reserve(1024); pix.reserve(65536);
        { std::lock_guard<std::mutex> l(m); q.push_back(std::move(b)); }
        work_cv.notify_all();
    }
    void wait() {
        submit();
        if (T == 0) return;
        std::unique_lock<std::mutex> l(m);
        done_cv.wait(l, [&] { return q.empty(); });
    }
    void discard() { wait(); rows.clear(); pix.clear(); p_hi = nullptr; }
};

// The cels of one picture of one screen, logged as they were drawn into its
// shadow: each row as the workers draw it, which source row it came from, and
// per cel a key (artwork, palette, format) and its four outer corners.
Gfx::Gfx(Memory& mem) : mem_(mem), frame_(kWidth * kHeight, 0), pool_(std::make_unique<HiPool>()) {}
Gfx::~Gfx() = default;

void Gfx::hi_wait() {
    static const bool stats = std::getenv("TDORT_HI_STATS") != nullptr;
    if (!stats) { pool_->wait(); return; }
    static uint64_t n = 0; static double total = 0;
    const auto t0 = std::chrono::steady_clock::now();
    pool_->wait();
    total += std::chrono::duration<double, std::milli>(std::chrono::steady_clock::now() - t0).count();
    if (++n % 200 == 0) std::fprintf(stderr, "hi_wait: %llu waits, %.0f ms waiting\n", (unsigned long long)n, total);
}

void Gfx::drop_shadows() { pool_->discard(); shadows_.clear(); cur_ = -1; }

uint16_t Gfx::process_state(const PixelState& p, uint16_t fb, uint32_t pixc, uint32_t flags) {
    Pixel px{};
    px.color = p.color; px.pmode = p.pmode; px.amv_r = p.amv_r; px.amv_g = p.amv_g; px.amv_b = p.amv_b; px.msb = p.msb;
    return process_with(px, fb, pixc, flags);
}


// ---- framebuffer access (LR form: line pairs share 32-bit words) -----------

uint16_t Gfx::fb_read(const CelTarget& t, int x, int y) const {
    const uint32_t base = t.read_buffer ? t.read_buffer : t.buffer;
    const int32_t w = t.read_buffer && t.read_width > 0 ? t.read_width : t.width;
    uint32_t a = base + uint32_t(((y >> 1) * w + x) * 4 + ((y & 1) ? 2 : 0));
    return rd16(mem_, a);
}

// Debugging aid: TDORT_PIXEL_WHO=<hex address of a frame-buffer pixel> logs
// each cel write to it (the cel's flags, PLUT pointer and the value).
static uint32_t g_pixel_who = [] { const char* e = std::getenv("TDORT_PIXEL_WHO"); return e ? uint32_t(std::strtoul(e, nullptr, 16)) : 0u; }();

void Gfx::fb_write(const CelTarget& t, int x, int y, uint16_t v) {
    uint32_t a = t.buffer + uint32_t(((y >> 1) * t.width + x) * 4 + ((y & 1) ? 2 : 0));
    if (a >= kRamEnd - 1) return;   // also catches addresses that wrap past 4 GB
    if (g_pixel_who && a == g_pixel_who)
        std::fprintf(stderr, "pixel %08X <- %04X by cel fl=%08X pre0=%08X pixc=%08X plut[0..3]=%04X %04X %04X %04X\n", a, v, r_.flags, r_.pre0,
                     r_.pixc, r_.plut[0], r_.plut[1], r_.plut[2], r_.plut[3]);
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

uint16_t Gfx::process(const Pixel& p, uint16_t fb) const { return process_with(p, fb, r_.pixc, r_.flags); }

// The pixel processor's result for one 5-bit channel depends only on the
// control half-word, the PXOR flag, the cel's and the frame buffer's channel
// values and the channel's multiplier: each control value in use gets a table
// of all 9 x 32 x 32 cases, made the first time it is needed (by any thread).
// TDORT_OLD_PIXC_TABLES=1: compute every pixel as before.
static uint8_t blend_channel(uint32_t ctl, bool pxor, int32_t pc, int32_t fc, uint32_t amv) {
    const bool prim_fb = ctl & 0x8000;
    const uint32_t ms = (ctl >> 13) & 3, mf = ((ctl >> 10) & 7) + 1, sf = (ctl >> 8) & 3;
    const uint32_t div = sf == 0 ? 16 : (1u << sf);
    const uint32_t s2 = (ctl >> 6) & 3, av = (ctl >> 1) & 0x1F;
    const bool two_d = ctl & 1;
    const bool invert = av & 1, wrap = av & 4;
    const uint32_t sdv_code = (av >> 3) & 3;
    const uint32_t sdv = sdv_code == 1 ? 2 : sdv_code == 2 ? 4 : 1;
    const int32_t prim = prim_fb ? fc : pc;
    int32_t mult;
    if (ms == 0) mult = int32_t(mf);
    else if (ms == 1) mult = amv ? int32_t(amv) : int32_t(mf);
    else mult = (pc >> 2) + 1;
    const int32_t a = prim * mult / int32_t(div);
    int32_t sec = 0;
    if (s2 == 1) sec = int32_t(av);
    else if (s2 == 2) sec = fc / int32_t(sdv);
    else if (s2 == 3) sec = pc / int32_t(sdv);
    int32_t res;
    if (pxor) res = a ^ sec;
    else if (s2 >= 2 && invert) res = a - sec;
    else res = a + sec;
    if (two_d) res >>= 1;
    res = wrap ? (res & 31) : std::clamp(res, 0, 31);
    return uint8_t(res);
}

static const uint8_t* make_blend_table(uint32_t ctl, bool pxor);
static inline const uint8_t* blend_table(uint32_t ctl, bool pxor) {
    // A cel uses one or two control values: remember the last one per thread.
    thread_local uint32_t last_key = ~0u; thread_local const uint8_t* last = nullptr;
    const uint32_t key = (ctl & 0xFFFF) | (pxor ? 0x10000u : 0u);
    if (key != last_key) { last = make_blend_table(ctl, pxor); last_key = key; }
    return last;
}
static const uint8_t* make_blend_table(uint32_t ctl, bool pxor) {
    static std::atomic<const uint8_t*> tables[2][65536];
    std::atomic<const uint8_t*>& slot = tables[pxor ? 1 : 0][ctl & 0xFFFF];
    const uint8_t* t = slot.load(std::memory_order_acquire);
    if (t) return t;
    uint8_t* n = new uint8_t[9 * 32 * 32];
    for (uint32_t m = 0; m < 9; ++m)
        for (int32_t pc = 0; pc < 32; ++pc)
            for (int32_t fc = 0; fc < 32; ++fc) n[(m * 32 + uint32_t(pc)) * 32 + uint32_t(fc)] = blend_channel(ctl, pxor, pc, fc, m);
    const uint8_t* expected = nullptr;
    if (slot.compare_exchange_strong(expected, n, std::memory_order_acq_rel)) return n;
    delete[] n;                           // another thread made it first
    return expected;
}

uint16_t Gfx::process_with(const Pixel& p, uint16_t fb, uint32_t pixc, uint32_t flags) {
    const uint32_t ctl = p.pmode ? (pixc >> 16) & 0xFFFF : pixc & 0xFFFF;
    static const bool old_tables = std::getenv("TDORT_OLD_PIXC_TABLES") != nullptr;
    uint16_t out;
    if (ctl == 0x1F40 || ctl == 0x1F00) {
        out = p.color;          // normal mode: cel colour unchanged
    } else if (!old_tables) {
        const uint8_t* t = blend_table(ctl, (flags & kPxor) != 0);
        auto ch = [&](int shift, uint8_t amv) {
            return uint32_t(t[(uint32_t(std::min<uint8_t>(amv, 8)) * 32 + ((p.color >> shift) & 31)) * 32 + ((fb >> shift) & 31)]) << shift;
        };
        out = uint16_t(ch(10, p.amv_r) | ch(5, p.amv_g) | ch(0, p.amv_b));
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
            if (flags & kPxor) res = a ^ sec;
            else if (s2 >= 2 && invert) res = a - sec;
            else res = a + sec;
            if (two_d) res >>= 1;
            res = wrap ? (res & 31) : std::clamp(res, 0, 31);
            out = uint16_t(out | (res << shift));
        }
    }
    if (out == 0 && !(flags & kNoBlk)) out = 0x0400;   // keep "black" distinct from background
    static const bool old_msb = std::getenv("TDORT_OLD_FB_MSB") != nullptr;
    if (p.msb && !old_msb) out |= 0x8000;
    return out;
}

static void make_row_blend(RowBlend& b, uint32_t ctl, uint32_t flags) {
    static const bool old_tables = std::getenv("TDORT_OLD_PIXC_TABLES") != nullptr;
    static const bool old_msb = std::getenv("TDORT_OLD_FB_MSB") != nullptr;
    b = RowBlend{};
    if (old_tables) { b.slow = true; return; }
    b.direct = ctl == 0x1F40 || ctl == 0x1F00;
    if (!b.direct) b.t = blend_table(ctl, (flags & kPxor) != 0);
    b.fix_black = !(flags & kNoBlk);
    b.old_msb = old_msb;
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
// The loops are bounded by the clip window and the bitmap (what plot() would
// refuse), the edge functions are stepped rather than recomputed, and the
// pixel processor is set up once for the quad. TDORT_OLD_FILL_QUAD=1: the
// plain version, pixel by pixel through plot().
void Gfx::fill_quad(const CelTarget& t, const Pixel& px, const int32_t (&x)[4], const int32_t (&y)[4]) {
    static const bool old_box0 = std::getenv("TDORT_OLD_QUAD_BOX") != nullptr;
    if (!old_box0) {
        // No pixel centre within the corners' extent (most quads: a source
        // pixel drawn smaller than a screen pixel): nothing to draw.
        const int32_t mnx = std::min(std::min(x[0], x[1]), std::min(x[2], x[3])), mxx = std::max(std::max(x[0], x[1]), std::max(x[2], x[3]));
        if (((int64_t(mnx) - 0x8000 + 0xFFFF) >> 16) > ((int64_t(mxx) - 0x8000) >> 16)) return;
        const int32_t mny = std::min(std::min(y[0], y[1]), std::min(y[2], y[3])), mxy = std::max(std::max(y[0], y[1]), std::max(y[2], y[3]));
        if (((int64_t(mny) - 0x8000 + 0xFFFF) >> 16) > ((int64_t(mxy) - 0x8000) >> 16)) return;
    }
    // Face culling: a mirrored/rotated-away pixel has the opposite winding.
    const int64_t area = int64_t(x[1] - x[0]) * (y[2] - y[0]) - int64_t(y[1] - y[0]) * (x[2] - x[0]);
    if (area == 0) return;
    if (area > 0 && !(r_.flags & kAcw)) return;
    if (area < 0 && !(r_.flags & kAccw)) return;
    static const bool old = std::getenv("TDORT_OLD_FILL_QUAD") != nullptr;
    if (old) { fill_quad_plain(t, px, x, y); return; }

    const int lx = std::max(t.clip_x, 0), ly = std::max(t.clip_y, 0);
    const int hx = std::min(t.clip_x + t.clip_w, t.width), hy = std::min(t.clip_y + t.clip_h, t.height);
    if (lx >= hx || ly >= hy) return;
    const uint32_t ctl = px.pmode ? (r_.pixc >> 16) & 0xFFFF : r_.pixc & 0xFFFF;
    const bool needs_fb = (ctl & 0x8000) || ((ctl >> 6) & 3) == 2;
    // Most quads are one source pixel scaled to under a destination pixel and
    // cover no pixel centre at all: the colour is worked out at the first
    // pixel written, not for every quad.
    uint16_t flat = 0;
    bool have_flat = false;
    auto put = [&](int xx, int yy) {
        if (needs_fb) fb_write(t, xx, yy, process(px, fb_read(t, xx, yy)));
        else {
            if (!have_flat) { flat = process(px, 0); have_flat = true; }
            fb_write(t, xx, yy, flat);
        }
        ++pixels_written;
    };
    static const bool old_box = std::getenv("TDORT_OLD_QUAD_BOX") != nullptr;

    // Axis-aligned fast path (the common unrotated sprite)
    if (y[0] == y[1] && y[2] == y[3] && x[0] == x[3] && x[1] == x[2]) {
        int32_t xa = std::min(x[0], x[1]), xb = std::max(x[0], x[1]);
        int32_t ya = std::min(y[0], y[3]), yb = std::max(y[0], y[3]);
        // pixel n is covered if its centre n+0.5 lies in [a, b)
        int px0 = int((int64_t(xa) + 0x7FFF) >> 16), px1 = int((int64_t(xb) + 0x7FFF) >> 16);
        int py0 = int((int64_t(ya) + 0x7FFF) >> 16), py1 = int((int64_t(yb) + 0x7FFF) >> 16);
        px0 = std::max(px0, lx); px1 = std::min(px1, hx); py0 = std::max(py0, ly); py1 = std::min(py1, hy);
        for (int yy = py0; yy < py1; ++yy)
            for (int xx = px0; xx < px1; ++xx) put(xx, yy);
        return;
    }
    int32_t minx = std::min({x[0], x[1], x[2], x[3]}), maxx = std::max({x[0], x[1], x[2], x[3]});
    int32_t miny = std::min({y[0], y[1], y[2], y[3]}), maxy = std::max({y[0], y[1], y[2], y[3]});
    // Only pixels whose centres (n + 0.5) lie within the corners' extent can
    // be inside; the box used to start a pixel early and end one late, and the
    // (usually one or two) pixels it adds failed the edge tests anyway.
    int bx0, bx1, by0, by1;
    if (old_box) {
        bx0 = std::max<int>(int(minx >> 16), lx); bx1 = std::min<int>(int(maxx >> 16) + 1, hx);
        by0 = std::max<int>(int(miny >> 16), ly); by1 = std::min<int>(int(maxy >> 16) + 1, hy);
    } else {
        bx0 = std::max<int>(int((int64_t(minx) - 0x8000 + 0xFFFF) >> 16), lx);
        bx1 = std::min<int>(int((int64_t(maxx) - 0x8000) >> 16) + 1, hx);
        by0 = std::max<int>(int((int64_t(miny) - 0x8000 + 0xFFFF) >> 16), ly);
        by1 = std::min<int>(int((int64_t(maxy) - 0x8000) >> 16) + 1, hy);
    }
    if (bx0 >= bx1 || by0 >= by1) return;
    const int64_t sign = area > 0 ? 1 : -1;
    if (!old_box && bx1 - bx0 == 1 && by1 - by0 == 1) {      // one candidate pixel: test it directly
        const int64_t cx = (int64_t(bx0) << 16) + 0x8000, cy = (int64_t(by0) << 16) + 0x8000;
        for (int e = 0; e < 4; ++e) {
            const int f = (e + 1) & 3;
            const int64_t ex = int64_t(x[f]) - x[e], ey = int64_t(y[f]) - y[e];
            if ((ex * (cy - y[e]) - ey * (cx - x[e])) * sign < 0) return;
        }
        put(bx0, by0);
        return;
    }
    // e(x, y) = ex * (cy - y[e]) - ey * (cx - x[e]), times the winding's sign.
    int64_t row_e[4], dx_e[4], dy_e[4];
    const int64_t cx0 = (int64_t(bx0) << 16) + 0x8000, cy0 = (int64_t(by0) << 16) + 0x8000;
    for (int e = 0; e < 4; ++e) {
        const int f = (e + 1) & 3;
        const int64_t ex = int64_t(x[f]) - x[e], ey = int64_t(y[f]) - y[e];
        row_e[e] = (ex * (cy0 - y[e]) - ey * (cx0 - x[e])) * sign;
        dx_e[e] = -ey * 65536 * sign;
        dy_e[e] = ex * 65536 * sign;
    }
    for (int yy = by0; yy < by1; ++yy) {
        int64_t e0 = row_e[0], e1 = row_e[1], e2 = row_e[2], e3 = row_e[3];
        for (int xx = bx0; xx < bx1; ++xx) {
            if ((e0 | e1 | e2 | e3) >= 0) put(xx, yy);
            e0 += dx_e[0]; e1 += dx_e[1]; e2 += dx_e[2]; e3 += dx_e[3];
        }
        for (int e = 0; e < 4; ++e) row_e[e] += dy_e[e];
    }
}

void Gfx::fill_quad_plain(const CelTarget& t, const Pixel& px, const int32_t (&x)[4], const int32_t (&y)[4]) {
    const int64_t area = int64_t(x[1] - x[0]) * (y[2] - y[0]) - int64_t(y[1] - y[0]) * (x[2] - x[0]);
    // Axis-aligned fast path (the common unrotated sprite)
    if (y[0] == y[1] && y[2] == y[3] && x[0] == x[3] && x[1] == x[2]) {
        int32_t xa = std::min(x[0], x[1]), xb = std::max(x[0], x[1]);
        int32_t ya = std::min(y[0], y[3]), yb = std::max(y[0], y[3]);
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
    // The cel engine fetches source data a 32-bit word at a time and ignores
    // the low two bits of the address. The Lost Files of Sherlock Holmes
    // draws its movies as small packed tiles whose pointers are often only
    // 2-byte aligned; read from the exact byte, every row was shifted and the
    // pictures turned to streaks. TDORT_OLD_CEL_SRC_ALIGN=1: use the exact address.
    static const bool old_align = std::getenv("TDORT_OLD_CEL_SRC_ALIGN") != nullptr;
    if (!old_align) src &= ~3u;
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

    // A pixel's decoding depends only on the cel (flags, preamble, PLUT): with
    // 8 bits or fewer each value is decoded once per cel, and 16-bit pixels
    // reuse the last value (runs of one colour). TDORT_OLD_CEL_DECODE=1: decode
    // every pixel.
    static const bool old_decode = std::getenv("TDORT_OLD_CEL_DECODE") != nullptr;
    const bool small_px = bpp <= 8 && !old_decode;
    Pixel dtab[256];
    uint8_t dval[256];
    if (small_px) std::memset(dval, 0, sizeof dval);
    uint32_t last_raw = ~0u;
    Pixel last_px{};
    auto dec = [&](uint32_t raw) -> Pixel {
        if (small_px) {
            raw &= 0xFF;
            if (!dval[raw]) { dtab[raw] = decode(raw, bpp, uncoded); dval[raw] = 1; }
            return dtab[raw];
        }
        if (old_decode) return decode(raw, bpp, uncoded);
        if (raw != last_raw) { last_px = decode(raw, bpp, uncoded); last_raw = raw; }
        return last_px;
    };

    uint32_t row_addr = src;
    for (int row = 0; row < rows; ++row) {
        const int64_t nox = ox + vx, noy = oy + vy, nhx = hx + r_.hddx, nhy = hy + r_.hddy;
        // The shadow's copy of this row (see HiRow).
        HiRow* hrow = nullptr;
        if (cur_ >= 0) {
            HiPool& hp = *pool_;
            hp.rows.push_back(HiRow{ox, oy, hx, hy, nox, noy, nhx, nhy, t.clip_x * 65536, t.clip_y * 65536,
                                    int16_t(std::clamp(t.clip_x, -32000, 32000)), int16_t(std::clamp(t.clip_y, -32000, 32000)),
                                    int16_t(std::clamp(t.clip_w, 0, 32000)), int16_t(std::clamp(t.clip_h, 0, 32000)),
                                    r_.pixc, r_.flags, uint32_t(hp.pix.size()), 0, uint8_t(scale_)});
            hrow = &hp.rows.back();
        }
        auto emit = [&](int col, uint32_t raw) {
            const Pixel px = dec(raw);
            if (px.transparent) return;
            if (hrow && col >= 0 && col < 65536) {
                pool_->pix.push_back(HiPix{uint16_t(col), px.color, uint8_t((px.pmode ? 1 : 0) | (px.msb ? 2 : 0)), px.amv_r, px.amv_g, px.amv_b});
                ++hrow->count;
            }
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
        if (hrow) {
            HiPool& hp = *pool_;
            if (!hrow->count) hp.rows.pop_back();
            else if (hp.pix.size() >= 262144) hp.submit();
        }
        ox = nox; oy = noy; hx = nhx; hy = nhy;
    }
    // The corner engine leaves its position after the last row: the next cel
    // starts there unless its CCB loads a new one (YOXY).
    r_.xpos = int32_t(ox >> 4); r_.ypos = int32_t(oy >> 4);
}

int Gfx::draw_cels(const CelTarget& t, uint32_t ccb) {
    int drawn = 0;
    // Blending against another buffer (SetReadAddress) is drawn at 1x only:
    // that buffer's hi-res copy isn't kept in step, and the destination's
    // copy picks the change up from memory (sync) before it is shown.
    const bool reads_other = t.read_buffer && t.read_buffer != t.buffer;
    cur_ = scale_ > 1 && !reads_other ? shadow_for(t.buffer, t.width, t.height, true) : -1;
    if (cur_ >= 0) {
        sync(shadows_[size_t(cur_)]);
        Shadow& sh = shadows_[size_t(cur_)];
        pool_->target(sh.hi.data(), sh.w * scale_, sh.h * scale_);
    }
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
        if (cel_trace && (r_.flags & kLdPlut))
            std::fprintf(stderr, "    plut %08X: %04X %04X %04X %04X %04X %04X %04X %04X\n", plut, r_.plut[0], r_.plut[1], r_.plut[2],
                         r_.plut[3], r_.plut[4], r_.plut[5], r_.plut[6], r_.plut[7]);
        if (!(r_.flags & kSkip)) {
            const uint64_t before = pixels_written;
            draw_one(t, src); ++drawn;
            if (cel_trace) std::fprintf(stderr, "    wrote %llu pixels\n", (unsigned long long)(pixels_written - before));
        }
        if (r_.flags & kLast) break;
        uint32_t nxt = (r_.flags & kNpAbs) ? next : next_field + 4 + next;
        if (!next && (r_.flags & kNpAbs)) break;
        ccb = nxt;
    }
    cels_drawn += uint64_t(drawn);
    if (cur_ >= 0) pool_->submit();       // this call's quads go to the workers
    cur_ = -1;
    return drawn;
}

} // namespace tdort
