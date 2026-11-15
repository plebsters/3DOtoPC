// tdort/aif.h - ARM Image Format (AIF) parsing, shared by converter and runtime.
// 3DO executables (LaunchMe and friends) are executable AIF images for a
// big-endian ARM60. The 128-byte header is itself code: it runs the optional
// decompress / self-relocate / zero-init steps and then BLs to the entry.
#pragma once
#include <cstdint>
#include <cstring>
#include <string>
#include <vector>

namespace tdort {

struct AifImage {
    bool     big_endian = true;
    uint32_t image_base = 0;
    uint32_t entry = 0;          // target of the BL at header+0x0C
    uint32_t ro_size = 0, rw_size = 0, debug_size = 0, zi_size = 0;
    uint32_t address_mode = 0;   // 26 or 32
    bool     compressed = false, self_relocating = false, zero_init_code = false;
    uint32_t file_size = 0;
    std::vector<uint8_t> bytes;  // header + RO + RW (+ tail if self-relocating/compressed), guest byte order

    uint32_t load_size() const { return uint32_t(bytes.size()); }
    uint32_t end() const { return image_base + load_size() + zi_size; }
};

inline uint32_t aif_word(const uint8_t* p, bool be) {
    return be ? (uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3])
              : (uint32_t(p[3]) << 24 | uint32_t(p[2]) << 16 | uint32_t(p[1]) << 8 | p[0]);
}

inline bool aif_is_nop_or_bl(uint32_t w) {
    return w == 0xE1A00000u || (w & 0xFF000000u) == 0xEB000000u;
}

inline bool parse_aif(const std::vector<uint8_t>& file, AifImage& out, std::string& err) {
    if (file.size() < 128) { err = "file too small for an AIF header"; return false; }
    const uint8_t* h = file.data();
    // Detect byte order from the header's code words (NOP or BL).
    bool be;
    if (aif_is_nop_or_bl(aif_word(h, true)) && aif_is_nop_or_bl(aif_word(h + 0x0C, true))) be = true;
    else if (aif_is_nop_or_bl(aif_word(h, false)) && aif_is_nop_or_bl(aif_word(h + 0x0C, false))) be = false;
    else { err = "not an executable AIF image"; return false; }

    auto W = [&](uint32_t off) { return aif_word(h + off, be); };
    out.big_endian      = be;
    out.compressed      = W(0x00) != 0xE1A00000u;
    out.self_relocating = W(0x04) != 0xE1A00000u;
    out.zero_init_code  = W(0x08) != 0xE1A00000u;
    out.ro_size     = W(0x14);
    out.rw_size     = W(0x18);
    out.debug_size  = W(0x1C);
    out.zi_size     = W(0x20);
    out.image_base  = W(0x28);
    out.address_mode = W(0x30) & 0xFF;

    uint32_t bl = W(0x0C);
    if ((bl & 0xFF000000u) != 0xEB000000u) { err = "AIF entry word is not a BL"; return false; }
    int32_t off = int32_t(bl << 8) >> 6;
    out.entry = out.image_base + 0x0C + 8 + uint32_t(off);

    // ro_size includes the header in executable AIF. Self-relocating and
    // compressed images keep their relocation / decompression code and data
    // after the RW area, so those need the whole file in memory.
    uint64_t load = uint64_t(out.ro_size) + out.rw_size;
    if (out.compressed || out.self_relocating) load = file.size();
    out.file_size = uint32_t(file.size());
    if (load > file.size()) load = file.size();   // tolerate truncated debug data
    out.bytes.assign(file.begin(), file.begin() + size_t(load));
    if (!be) {   // normalise to guest (big-endian) word order
        for (size_t i = 0; i + 3 < out.bytes.size(); i += 4) {
            std::swap(out.bytes[i], out.bytes[i + 3]);
            std::swap(out.bytes[i + 1], out.bytes[i + 2]);
        }
    }
    return true;
}

inline std::string fnv1a64_hex(const std::vector<uint8_t>& data) {
    uint64_t h = 1469598103934665603ull;
    for (uint8_t b : data) { h ^= b; h *= 1099511628211ull; }
    char buf[17];
    for (int i = 15; i >= 0; --i) { buf[i] = "0123456789abcdef"[h & 15]; h >>= 4; }
    buf[16] = 0;
    return buf;
}

} // namespace tdort
