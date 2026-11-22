// tdo/opera_fs.h - reader for the 3DO "Opera" CD filesystem.
// Accepts plain 2048-byte-sector images (.iso) and raw 2352-byte Mode 1
// images (.bin); the sector format is detected automatically.
#pragma once
#include <cstdint>
#include <cstdio>
#include <functional>
#include <string>
#include <vector>

namespace tdo {

struct OperaEntry {
    std::string path;        // "/Folder/File"
    bool        is_dir = false;
    uint32_t    id = 0;
    char        type[5] = {};
    uint32_t    block_size = 2048;
    uint32_t    byte_count = 0;
    uint32_t    block_count = 0;
    std::vector<uint32_t> avatars;   // start blocks of each copy of the data
};

struct OperaVolume {
    std::string label, comment;
    uint32_t    volume_id = 0, block_size = 2048, block_count = 0;
    uint32_t    root_id = 0, root_blocks = 0, root_block_size = 2048;
    std::vector<uint32_t> root_avatars;
};

class OperaDisc {
public:
    ~OperaDisc();
    bool open(const std::string& image_path, std::string& err);
    const OperaVolume& volume() const { return vol_; }
    const std::vector<OperaEntry>& entries() const { return entries_; }
    const OperaEntry* find(const std::string& path) const;   // case-insensitive

    bool read_file(const OperaEntry& e, std::vector<uint8_t>& out, std::string& err);
    bool extract_all(const std::string& out_dir, std::string& err,
                     const std::function<void(const OperaEntry&, size_t, size_t)>& progress = {});

    // Entries extract_all left out: a second name that differs only in case
    // (the console finds the first), and anything inside such a folder.
    const std::vector<std::string>& skipped() const { return skipped_; }

    // Read `count` logical 2048-byte blocks.
    bool read_blocks(uint32_t block, uint32_t count, std::vector<uint8_t>& out);

private:
    bool read_dir(const std::string& prefix, uint32_t first_block, uint32_t blocks,
                  uint32_t block_size, int depth, std::string& err);

    FILE* f_ = nullptr;
    std::vector<std::string> skipped_;
    uint32_t sector_size_ = 2048, sector_data_off_ = 0;
    OperaVolume vol_;
    std::vector<OperaEntry> entries_;
};

} // namespace tdo
