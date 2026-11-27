// cover.cpp - cover art for a converted game: found, saved as cover.png, and
// made into the game's .exe icon. Everything is local: the picture comes from
// the user (an image beside the disc file) or from the disc itself.
#include "cover.h"

#include <algorithm>
#include <cmath>
#include <cstdint>
#include <cstring>
#include <fstream>

#define STBI_ONLY_PNG
#define STBI_ONLY_JPEG
#define STBI_ONLY_BMP
#define STBI_NO_STDIO_FAILURE_MESSAGES
#define STB_IMAGE_STATIC
#define STB_IMAGE_IMPLEMENTATION
#include "../third_party/stb/stb_image.h"
#define STB_IMAGE_WRITE_STATIC
#define STB_IMAGE_WRITE_IMPLEMENTATION
#include "../third_party/stb/stb_image_write.h"

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#endif

namespace fs = std::filesystem;

namespace tdo {

namespace {

bool read_whole(const fs::path& p, std::vector<uint8_t>& out) {
    std::ifstream f(p, std::ios::binary);
    if (!f) return false;
    f.seekg(0, std::ios::end);
    const std::streamoff n = f.tellg();
    if (n <= 0 || n > (64 << 20)) return false;
    out.resize(size_t(n));
    f.seekg(0);
    return bool(f.read(reinterpret_cast<char*>(out.data()), n));
}

bool decode_image(const std::vector<uint8_t>& bytes, Image& img) {
    int w = 0, h = 0, n = 0;
    unsigned char* px = stbi_load_from_memory(bytes.data(), int(bytes.size()), &w, &h, &n, 4);
    if (!px) return false;
    if (w < 16 || h < 16 || w > 8192 || h > 8192) { stbi_image_free(px); return false; }
    img.w = w; img.h = h;
    img.rgba.assign(px, px + size_t(w) * size_t(h) * 4);
    stbi_image_free(px);
    return true;
}

uint64_t fnv1a(const uint8_t* p, size_t n) {
    uint64_t h = 0xcbf29ce484222325ull;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 0x100000001b3ull;
    return h;
}

// Area-average resize (only ever used to shrink).
Image resize(const Image& s, int w, int h) {
    Image d; d.w = w; d.h = h; d.rgba.assign(size_t(w) * size_t(h) * 4, 0);
    const double sx = double(s.w) / w, sy = double(s.h) / h;
    for (int y = 0; y < h; ++y) {
        const double y0 = y * sy, y1 = std::min<double>(s.h, (y + 1) * sy);
        for (int x = 0; x < w; ++x) {
            const double x0 = x * sx, x1 = std::min<double>(s.w, (x + 1) * sx);
            double acc[4] = {0, 0, 0, 0}, area = 0;
            for (int yy = int(y0); yy < int(std::ceil(y1)); ++yy) {
                const double wy = std::min<double>(yy + 1, y1) - std::max<double>(yy, y0);
                if (wy <= 0) continue;
                for (int xx = int(x0); xx < int(std::ceil(x1)); ++xx) {
                    const double wx = std::min<double>(xx + 1, x1) - std::max<double>(xx, x0);
                    if (wx <= 0) continue;
                    const uint8_t* p = &s.rgba[(size_t(yy) * s.w + xx) * 4];
                    const double wgt = wx * wy, a = p[3] / 255.0;
                    acc[0] += p[0] * a * wgt; acc[1] += p[1] * a * wgt; acc[2] += p[2] * a * wgt;
                    acc[3] += a * wgt; area += wgt;
                }
            }
            uint8_t* q = &d.rgba[(size_t(y) * w + x) * 4];
            if (acc[3] > 0) for (int c = 0; c < 3; ++c) q[c] = uint8_t(std::clamp(acc[c] / acc[3] + 0.5, 0.0, 255.0));
            q[3] = area > 0 ? uint8_t(std::clamp(acc[3] / area * 255.0 + 0.5, 0.0, 255.0)) : 0;
        }
    }
    return d;
}

// The picture fitted into a square (transparent bands), size x size.
Image fit_square(const Image& s, int size) {
    const double k = std::min(double(size) / s.w, double(size) / s.h);
    const int w = std::max(1, int(s.w * k + 0.5)), h = std::max(1, int(s.h * k + 0.5));
    const Image r = (w == s.w && h == s.h) ? s : resize(s, w, h);
    Image d; d.w = d.h = size; d.rgba.assign(size_t(size) * size * 4, 0);
    const int ox = (size - w) / 2, oy = (size - h) / 2;
    for (int y = 0; y < h; ++y)
        std::memcpy(&d.rgba[(size_t(y + oy) * size + ox) * 4], &r.rgba[size_t(y) * w * 4], size_t(w) * 4);
    return d;
}

void png_append(void* ctx, void* data, int size) {
    auto* v = static_cast<std::vector<uint8_t>*>(ctx);
    v->insert(v->end(), static_cast<uint8_t*>(data), static_cast<uint8_t*>(data) + size);
}

std::vector<uint8_t> encode_png(const Image& img) {
    std::vector<uint8_t> out;
    stbi_write_png_to_func(png_append, &out, img.w, img.h, 4, img.rgba.data(), img.w * 4);
    return out;
}

const int kIconSizes[] = {256, 48, 32, 16};

}  // namespace

// BannerScreen: a 3DO disc's title picture, shown while the game boots. The
// file is a small header ("\x01APPSCRN") and a 320x240 picture of 16-bit
// pixels (0RRRRRGGGGGBBBBB, big-endian) stored the way the console's frame
// buffer holds it: two lines at a time, a word from the even line then the
// word below it from the odd one.
bool decode_banner(const std::vector<uint8_t>& file, Image& img) {
    constexpr size_t kPix = 320 * 240 * 2;
    if (file.size() < kPix + 8 || std::memcmp(file.data() + 1, "APPSCRN", 7) != 0) return false;
    const uint8_t* p = file.data() + file.size() - kPix;
    // Panasonic's developer-kit sample ("Fictional Developer presents Bogus
    // Title"), left on several discs: not this game's art.
    if (fnv1a(p, kPix) == 0x97e0ac23ea639662ull) return false;
    img.w = 320; img.h = 240; img.rgba.resize(size_t(320) * 240 * 4);
    uint32_t lo = 0xFFFF, hi = 0;
    for (int y = 0; y < 240; ++y)
        for (int x = 0; x < 320; ++x) {
            const size_t o = (size_t(y >> 1) * 320 + size_t(x)) * 4 + size_t(y & 1) * 2;
            const uint32_t v = (uint32_t(p[o]) << 8) | p[o + 1];
            lo = std::min(lo, v & 0x7FFF); hi = std::max(hi, v & 0x7FFF);
            const uint32_t r = (v >> 10) & 31, g = (v >> 5) & 31, b = v & 31;
            uint8_t* q = &img.rgba[(size_t(y) * 320 + size_t(x)) * 4];
            q[0] = uint8_t(r << 3 | r >> 2); q[1] = uint8_t(g << 3 | g >> 2); q[2] = uint8_t(b << 3 | b >> 2); q[3] = 255;
        }
    return lo != hi;   // one flat colour is no picture
}

std::string find_user_cover(const fs::path& disc_path, std::vector<uint8_t>& bytes, Image& img) {
    std::error_code ec;
    const fs::path dir = fs::is_directory(disc_path, ec) ? disc_path : disc_path.parent_path();
    std::vector<fs::path> cands;
    if (!fs::is_directory(disc_path, ec))
        for (const char* ext : {".png", ".jpg", ".jpeg"}) cands.push_back(fs::path(disc_path).replace_extension(ext));
    for (const char* n : {"cover.png", "cover.jpg", "cover.jpeg", "folder.jpg", "folder.png", "boxart.png", "boxart.jpg"})
        cands.push_back(dir / n);
    for (const fs::path& c : cands) {
        if (!fs::is_regular_file(c, ec)) continue;
        if (read_whole(c, bytes) && decode_image(bytes, img)) return c.filename().u8string();
    }
    return {};
}

bool load_image(const fs::path& file, Image& img) {
    std::vector<uint8_t> bytes;
    return read_whole(file, bytes) && decode_image(bytes, img);
}

bool save_cover(const Image& img, const fs::path& png_path) {
    const std::vector<uint8_t> png = encode_png(img);
    if (png.empty()) return false;
    std::ofstream f(png_path, std::ios::binary);
    return f && bool(f.write(reinterpret_cast<const char*>(png.data()), std::streamsize(png.size())));
}

std::vector<std::vector<uint8_t>> icon_images(const Image& img) {
    std::vector<std::vector<uint8_t>> out;
    for (int s : kIconSizes) out.push_back(encode_png(fit_square(img, s)));
    return out;
}

bool set_exe_icon(const fs::path& exe, const Image& img, std::string& err) {
#if defined(_WIN32)
    const auto pngs = icon_images(img);
    HANDLE h = BeginUpdateResourceW(exe.wstring().c_str(), FALSE);
    if (!h) { err = "cannot open the .exe to add its icon (error " + std::to_string(GetLastError()) + ")"; return false; }
    const WORD lang = MAKELANGID(LANG_NEUTRAL, SUBLANG_NEUTRAL);
    // RT_GROUP_ICON: a header and one 14-byte entry per image (RT_ICON 1..n).
    std::vector<uint8_t> grp(6 + 14 * pngs.size(), 0);
    grp[2] = 1; grp[4] = uint8_t(pngs.size());
    bool ok = true;
    for (size_t i = 0; i < pngs.size(); ++i) {
        const int s = kIconSizes[i];
        uint8_t* e = &grp[6 + 14 * i];
        e[0] = uint8_t(s >= 256 ? 0 : s); e[1] = e[0];          // 0 means 256
        e[4] = 1; e[6] = 32;                                    // planes, bits per pixel
        const uint32_t n = uint32_t(pngs[i].size());
        std::memcpy(e + 8, &n, 4);
        const uint16_t id = uint16_t(i + 1);
        std::memcpy(e + 12, &id, 2);
        ok = ok && UpdateResourceW(h, MAKEINTRESOURCEW(3) /*RT_ICON*/, MAKEINTRESOURCEW(id), lang,
                                   const_cast<uint8_t*>(pngs[i].data()), n);
    }
    ok = ok && UpdateResourceW(h, MAKEINTRESOURCEW(14) /*RT_GROUP_ICON*/, MAKEINTRESOURCEW(1), lang, grp.data(), DWORD(grp.size()));
    if (!EndUpdateResourceW(h, ok ? FALSE : TRUE) || !ok) {
        err = "could not write the icon into the .exe (error " + std::to_string(GetLastError()) + ")";
        return false;
    }
    return true;
#else
    (void)exe; (void)img;
    err = "icons are added only on Windows";
    return false;
#endif
}

std::vector<uint8_t> make_ico(const Image& img) {
    const auto pngs = icon_images(img);
    std::vector<uint8_t> ico(6 + 16 * pngs.size(), 0);
    ico[2] = 1; ico[4] = uint8_t(pngs.size());
    uint32_t off = uint32_t(ico.size());
    for (size_t i = 0; i < pngs.size(); ++i) {
        const int s = kIconSizes[i];
        uint8_t* e = &ico[6 + 16 * i];
        e[0] = uint8_t(s >= 256 ? 0 : s); e[1] = e[0];
        e[4] = 1; e[6] = 32;
        const uint32_t n = uint32_t(pngs[i].size());
        std::memcpy(e + 8, &n, 4);
        std::memcpy(e + 12, &off, 4);
        off += n;
    }
    for (const auto& p : pngs) ico.insert(ico.end(), p.begin(), p.end());
    return ico;
}

}  // namespace tdo
