// tdo/recomp.h - ARM60 disassembly, control-flow analysis and C++ emission.
#pragma once
#include <cstdint>
#include <map>
#include <set>
#include <string>
#include <vector>
#include "tdort/aif.h"

namespace tdo {

std::string disasm(uint32_t insn, uint32_t pc);

struct Block {
    uint32_t start = 0;
    uint32_t count = 0;   // instructions
};

struct AnalysisOptions {
    bool scan_prologues = true;           // treat STMFD sp!,{..,lr} as function starts
    std::vector<uint32_t> extra_roots;    // e.g. from the runtime's interp_hits.txt
    uint32_t max_block_insns = 256;
};

struct Analysis {
    std::map<uint32_t, Block> blocks;     // keyed by start address
    std::set<uint32_t> indirect_sites;    // instructions with unknown targets
    uint32_t code_insns = 0;
};

Analysis analyze(const tdort::AifImage& img, const AnalysisOptions& opt);

struct EmitOptions {
    std::string game_name = "3DO Game";
    uint32_t blocks_per_file = 250;      // small files keep compiler memory low
};

// Writes recomp_*.cpp and recomp_table.cpp into out_dir. Returns file list.
std::vector<std::string> emit_cpp(const tdort::AifImage& img, const Analysis& a,
                                  const EmitOptions& opt, const std::string& out_dir);

// Writes a listing (disassembly with block markers) for inspection.
void write_listing(const tdort::AifImage& img, const Analysis& a, const std::string& path);

std::vector<uint32_t> read_hints(const std::string& path);

} // namespace tdo
