// opera_fs.cpp - Opera filesystem reader.
//
// Volume header (block 0, big-endian):
//   0x00 u8 record type (1)   0x01 5 x 0x5A sync   0x06 u8 version (1)
//   0x07 u8 flags   0x08 comment[32]   0x28 label[32]
//   0x48 volume id  0x4C block size  0x50 block count
//   0x54 root id    0x58 root blocks 0x5C root block size
//   0x60 root last avatar index      0x64 root avatars[]
// Directory block: next(i32) prev(i32) flags first_free first_entry_offset,
// then entries: flags id type[4] block_size byte_count block_count burst gap
// name[32] last_avatar_index avatars[last+1].
#include "tdort/host_name.h"
#include "tdo/opera_fs.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>
#include <map>

namespace fs = std::filesystem;

namespace tdo {

static uint32_t be32(const uint8_t* p) {
    return uint32_t(p[0]) << 24 | uint32_t(p[1]) << 16 | uint32_t(p[2]) << 8 | p[3];
}
static std::string cstr(const uint8_t* p, size_t n) {
    size_t len = 0; while (len < n && p[len]) ++len;
    return std::string(reinterpret_cast<const char*>(p), len);
}
static bool is_volume_header(const uint8_t* p) {
    return p[0] == 0x01 && p[1] == 0x5A && p[2] == 0x5A && p[3] == 0x5A && p[4] == 0x5A &&
           p[5] == 0x5A && p[6] == 0x01;
}

OperaDisc::~OperaDisc() { if (f_) std::fclose(f_); }

bool OperaDisc::read_blocks(uint32_t block, uint32_t count, std::vector<uint8_t>& out) {
    out.resize(size_t(count) * 2048);
    for (uint32_t i = 0; i < count; ++i) {
        uint64_t off = uint64_t(block + i) * sector_size_ + sector_data_off_;
#if defined(_WIN32)
        if (_fseeki64(f_, int64_t(off), SEEK_SET) != 0) return false;
#else
        if (fseeko(f_, off_t(off), SEEK_SET) != 0) return false;
#endif
        if (std::fread(&out[size_t(i) * 2048], 1, 2048, f_) != 2048) return false;
    }
    return true;
}

bool OperaDisc::open(const std::string& path, std::string& err) {
#if defined(_WIN32)
    f_ = _wfopen(fs::u8path(path).wstring().c_str(), L"rb");
#else
    f_ = std::fopen(path.c_str(), "rb");
#endif
    if (!f_) { err = "cannot open disc image: " + path; return false; }

    uint8_t probe[2352 + 32] = {};
    size_t got = std::fread(probe, 1, sizeof probe, f_);
    if (got >= 7 && is_volume_header(probe))            { sector_size_ = 2048; sector_data_off_ = 0; }
    else if (got >= 23 && is_volume_header(probe + 16)) { sector_size_ = 2352; sector_data_off_ = 16; }
    else { err = "no Opera volume header found (not a 3DO disc image?)"; return false; }

    std::vector<uint8_t> b;
    if (!read_blocks(0, 1, b)) { err = "cannot read volume header"; return false; }
    const uint8_t* h = b.data();
    vol_.comment = cstr(h + 0x08, 32);
    vol_.label = cstr(h + 0x28, 32);
    vol_.volume_id = be32(h + 0x48);
    vol_.block_size = be32(h + 0x4C);
    vol_.block_count = be32(h + 0x50);
    vol_.root_id = be32(h + 0x54);
    vol_.root_blocks = be32(h + 0x58);
    vol_.root_block_size = be32(h + 0x5C);
    uint32_t last = be32(h + 0x60);
    if (vol_.block_size != 2048 || vol_.root_block_size != 2048 || last > 7) {
        err = "unsupported Opera volume parameters"; return false;
    }
    for (uint32_t i = 0; i <= last; ++i) vol_.root_avatars.push_back(be32(h + 0x64 + 4 * i));

    entries_.clear();
    return read_dir("", vol_.root_avatars[0], vol_.root_blocks, vol_.root_block_size, 0, err);
}

bool OperaDisc::read_dir(const std::string& prefix, uint32_t first_block, uint32_t blocks,
                         uint32_t block_size, int depth, std::string& err) {
    if (depth > 32) { err = "directory nesting too deep"; return false; }
    if (block_size != 2048) { err = "unsupported directory block size"; return false; }
    for (uint32_t bi = 0; bi < blocks; ++bi) {
        std::vector<uint8_t> blk;
        if (!read_blocks(first_block + bi, 1, blk)) { err = "cannot read directory block"; return false; }
        uint32_t off = be32(&blk[16]);
        bool last_in_dir = false;
        while (off + 68 <= 2048) {
            const uint8_t* e = &blk[off];
            const uint32_t flags = be32(e);
            OperaEntry ent;
            ent.id = be32(e + 4);
            std::memcpy(ent.type, e + 8, 4);
            ent.block_size = be32(e + 12);
            ent.byte_count = be32(e + 16);
            ent.block_count = be32(e + 20);
            std::string name = cstr(e + 32, 32);
            const uint32_t last = be32(e + 64);
            if (last > 255 || off + 68 + 4 * (last + 1) > 2048) { err = "corrupt directory entry"; return false; }
            for (uint32_t a = 0; a <= last; ++a) ent.avatars.push_back(be32(e + 68 + 4 * a));
            ent.is_dir = (flags & 0xFF) == 0x07;
            ent.path = prefix + "/" + name;
            entries_.push_back(ent);
            if (ent.is_dir && !ent.avatars.empty()) {
                if (!read_dir(ent.path, ent.avatars[0], ent.block_count, ent.block_size, depth + 1, err))
                    return false;
            }
            off += 68 + 4 * (last + 1);
            if (flags & 0x80000000u) { last_in_dir = true; break; }
            if (flags & 0x40000000u) break;
        }
        if (last_in_dir) break;
    }
    return true;
}

const OperaEntry* OperaDisc::find(const std::string& path) const {
    auto lower = [](std::string s) { for (auto& ch : s) ch = char(std::tolower((unsigned char)ch)); return s; };
    std::string want = lower(path[0] == '/' ? path : "/" + path);
    for (auto& e : entries_) if (lower(e.path) == want) return &e;
    return nullptr;
}

bool OperaDisc::read_file(const OperaEntry& e, std::vector<uint8_t>& out, std::string& err) {
    if (e.is_dir || e.avatars.empty()) { err = "not a file: " + e.path; return false; }
    if (e.block_size != 2048) { err = "unsupported file block size: " + e.path; return false; }
    uint32_t blocks = (e.byte_count + 2047) / 2048;
    if (!read_blocks(e.avatars[0], blocks, out)) { err = "read error: " + e.path; return false; }
    out.resize(e.byte_count);
    return true;
}

bool OperaDisc::extract_all(const std::string& out_dir, std::string& err,
                            const std::function<void(const OperaEntry&, size_t, size_t)>& progress) {
    // The 3DO looks names up without regard to case and takes the first match
    // in a directory, while Windows can't hold two names that differ only in
    // case, nor a file and a folder of the same name. BattleSport has such a
    // pair under art/anims ("cannot write ...\eye.anim\new"). The first
    // entry wins, as on the console; the others (and anything inside a
    // folder that lost) are skipped and listed in skipped().
    auto lower = [](std::string s) { for (auto& ch : s) ch = char(std::tolower((unsigned char)ch)); return s; };
    std::map<std::string, bool> claimed;       // lower-case path -> is a directory
    skipped_.clear();
    size_t i = 0;
    for (auto& e : entries_) {
        ++i;
        const std::string key = lower(e.path);
        bool skip = claimed.count(key) != 0;
        for (size_t p = key.find('/', 1); !skip && p != std::string::npos; p = key.find('/', p + 1)) {
            auto it = claimed.find(key.substr(0, p));
            if (it == claimed.end() || !it->second) skip = true;   // parent lost or is a file
        }
        if (skip) { skipped_.push_back(e.path); continue; }
        claimed[key] = e.is_dir;
        // each name stored so that Windows accepts it (tdort/host_name.h)
        fs::path dst = fs::u8path(out_dir);
        {
            std::string comp;
            for (char ch : e.path.substr(1) + "/") {
                if (ch == '/') { if (!comp.empty()) dst /= fs::u8path(tdort::encode_host_name(comp)); comp.clear(); }
                else comp += ch;
            }
        }
        std::error_code ec;
        if (e.is_dir) { fs::create_directories(dst, ec); continue; }
        fs::create_directories(dst.parent_path(), ec);
        std::vector<uint8_t> data;
        if (!read_file(e, data, err)) return false;
        FILE* o = nullptr;
#if defined(_WIN32)
        o = _wfopen(dst.wstring().c_str(), L"wb");
#else
        o = std::fopen(dst.string().c_str(), "wb");
#endif
        if (!o) { err = "cannot write " + dst.string(); return false; }
        std::fwrite(data.data(), 1, data.size(), o);
        std::fclose(o);
        if (progress) progress(e, i, entries_.size());
    }
    return true;
}

} // namespace tdo
