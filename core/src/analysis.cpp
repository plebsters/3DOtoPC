// analysis.cpp - recursive-descent discovery of basic blocks.
//
// Safety property that makes aggressive discovery OK: a recompiled block runs
// exactly the same exec() semantics on the same instruction words as the
// interpreter would. Mistaking data for code costs only compile time; the
// block is never reached unless the pc genuinely lands there, and runtime
// write-tracking disables any block whose bytes change.
#include "tdo/recomp.h"
#include "tdort/arm_isa.h"
#include "tdort/memory.h"
#include <cstdio>
#include <deque>
#include <fstream>

namespace tdo {

using namespace tdort;

static bool plausible_code(uint32_t i) {
    Op op = classify(i);
    return op != Op::Undefined && op != Op::Coproc && cond_of(i) != 0xF;
}

Analysis analyze(const AifImage& img, const AnalysisOptions& opt) {
    Analysis a;
    const uint32_t lo = img.image_base, hi = img.image_base + img.load_size();
    auto in_image = [&](uint32_t addr) { return addr >= lo && addr + 4 <= hi && !(addr & 3); };
    auto word = [&](uint32_t addr) { return load_be32(&img.bytes[addr - lo]); };

    std::set<uint32_t> starts, code;
    std::deque<uint32_t> work;
    auto add_root = [&](uint32_t addr) {
        if (in_image(addr) && starts.insert(addr).second) work.push_back(addr);
    };

    add_root(img.image_base);   // AIF header code
    add_root(img.entry);
    for (uint32_t r : opt.extra_roots) add_root(r);
    if (opt.scan_prologues) {
        for (uint32_t addr = lo + 128; addr + 4 <= hi; addr += 4)
            if ((word(addr) & 0xFFFF4000u) == 0xE92D4000u) add_root(addr);   // stmfd sp!, {..., lr}
    }

    // Pass 1: discover reachable instructions and block leaders.
    while (!work.empty()) {
        uint32_t pc = work.front(); work.pop_front();
        for (;;) {
            if (!in_image(pc)) break;
            if (code.count(pc)) break;            // already scanned from here
            uint32_t i = word(pc);
            if (!plausible_code(i)) break;
            code.insert(pc);
            const bool always = cond_of(i) == 0xE;
            if (classify(i) == Op::Branch) {
                add_root(branch_target(i, pc));
                if (!always || is_bl(i)) add_root(pc + 4);
                break;
            }
            if (writes_pc(i)) {
                a.indirect_sites.insert(pc);
                // "mov lr, pc" right before a pc write is an indirect call
                // (Portfolio folio calls, CallFolio, cstartup): it returns here.
                const bool call = in_image(pc - 4) && word(pc - 4) == 0xE1A0E00Fu;
                if (!always || call) add_root(pc + 4);
                break;
            }
            if (classify(i) == Op::Swi) { add_root(pc + 4); break; }   // HLE may redirect
            pc += 4;
        }
    }

    // Pass 2: cut blocks at leaders and terminators.
    for (uint32_t s : starts) {
        if (!code.count(s)) continue;
        Block b; b.start = s;
        uint32_t pc = s;
        while (code.count(pc) && b.count < opt.max_block_insns) {
            uint32_t i = word(pc);
            ++b.count; pc += 4;
            if (classify(i) == Op::Branch || writes_pc(i) || classify(i) == Op::Swi) break;
            if (starts.count(pc)) break;
        }
        if (b.count) a.blocks[s] = b;
    }
    a.code_insns = uint32_t(code.size());
    return a;
}

std::vector<uint32_t> read_hints(const std::string& path) {
    std::vector<uint32_t> out;
    std::ifstream f(path);
    std::string line;
    while (std::getline(f, line)) {
        unsigned v = 0;
        if (std::sscanf(line.c_str(), "%x", &v) == 1) out.push_back(v);
    }
    return out;
}

void write_listing(const AifImage& img, const Analysis& a, const std::string& path) {
    std::ofstream o(path);
    o << "; " << a.blocks.size() << " blocks, " << a.code_insns << " instructions\n";
    for (auto& [start, b] : a.blocks) {
        o << "\nblock_" << std::hex << start << ":\n";
        for (uint32_t k = 0; k < b.count; ++k) {
            uint32_t pc = start + 4 * k;
            uint32_t i = load_be32(&img.bytes[pc - img.image_base]);
            char buf[32]; std::snprintf(buf, sizeof buf, "  %08X  %08X  ", pc, i);
            o << buf << disasm(i, pc) << (a.indirect_sites.count(pc) ? "    ; indirect" : "") << "\n";
        }
    }
}

} // namespace tdo
