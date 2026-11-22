// png_write.h - writes a 24-bit PNG from XRGB8888 pixels (used for F3 screenshots).
// Self-contained: the image data is stored in uncompressed deflate blocks, so
// no zlib is needed. A 320x240 screenshot is about 230 KB.
#pragma once
#include <cstdint>
#include <cstdio>
#include <vector>

namespace tdort {

inline uint32_t png_crc(const uint8_t* p, size_t n, uint32_t c = 0xFFFFFFFFu) {
    static uint32_t table[256];
    static bool ready = false;
    if (!ready) {
        for (uint32_t i = 0; i < 256; ++i) {
            uint32_t k = i;
            for (int j = 0; j < 8; ++j) k = (k & 1) ? 0xEDB88320u ^ (k >> 1) : k >> 1;
            table[i] = k;
        }
        ready = true;
    }
    for (size_t i = 0; i < n; ++i) c = table[(c ^ p[i]) & 0xFF] ^ (c >> 8);
    return c;
}

// PNG file bytes for a w x h image; each pixel is 0x00RRGGBB.
inline std::vector<uint8_t> png_encode(const uint32_t* px, int w, int h) {
    std::vector<uint8_t> out;
    auto be32 = [&](uint32_t v) { for (int s = 24; s >= 0; s -= 8) out.push_back(uint8_t(v >> s)); };
    auto chunk = [&](const char* type, const std::vector<uint8_t>& data) {
        be32(uint32_t(data.size()));
        const size_t start = out.size();
        out.insert(out.end(), type, type + 4);
        out.insert(out.end(), data.begin(), data.end());
        be32(png_crc(&out[start], out.size() - start) ^ 0xFFFFFFFFu);
    };
    static const uint8_t sig[8] = {0x89, 'P', 'N', 'G', 0x0D, 0x0A, 0x1A, 0x0A};
    out.insert(out.end(), sig, sig + 8);

    std::vector<uint8_t> ihdr;
    auto put32 = [](std::vector<uint8_t>& v, uint32_t x) { for (int s = 24; s >= 0; s -= 8) v.push_back(uint8_t(x >> s)); };
    put32(ihdr, uint32_t(w)); put32(ihdr, uint32_t(h));
    ihdr.insert(ihdr.end(), {8, 2, 0, 0, 0});      // 8-bit RGB, no interlace
    chunk("IHDR", ihdr);

    // Raw scanlines: filter byte 0, then R G B per pixel.
    std::vector<uint8_t> raw;
    raw.reserve(size_t(h) * (size_t(w) * 3 + 1));
    for (int y = 0; y < h; ++y) {
        raw.push_back(0);
        const uint32_t* row = px + size_t(y) * size_t(w);
        for (int x = 0; x < w; ++x) {
            raw.push_back(uint8_t(row[x] >> 16)); raw.push_back(uint8_t(row[x] >> 8)); raw.push_back(uint8_t(row[x]));
        }
    }
    // zlib stream with stored blocks (at most 65535 bytes each) and an Adler-32.
    std::vector<uint8_t> z = {0x78, 0x01};
    for (size_t pos = 0; pos < raw.size() || raw.empty();) {
        const size_t n = std::min<size_t>(65535, raw.size() - pos);
        const bool last = pos + n >= raw.size();
        z.push_back(last ? 1 : 0);
        z.push_back(uint8_t(n)); z.push_back(uint8_t(n >> 8));
        z.push_back(uint8_t(~n)); z.push_back(uint8_t(~n >> 8));
        z.insert(z.end(), raw.begin() + long(pos), raw.begin() + long(pos + n));
        pos += n;
        if (last) break;
    }
    uint32_t a = 1, b = 0;
    for (uint8_t v : raw) { a = (a + v) % 65521; b = (b + a) % 65521; }
    put32(z, (b << 16) | a);
    chunk("IDAT", z);
    chunk("IEND", {});
    return out;
}

// Writes the PNG to a file (path in the platform's narrow or wide form is the caller's business).
inline bool png_write_file(std::FILE* f, const uint32_t* px, int w, int h) {
    if (!f) return false;
    const std::vector<uint8_t> data = png_encode(px, w, h);
    return std::fwrite(data.data(), 1, data.size(), f) == data.size();
}

} // namespace tdort
