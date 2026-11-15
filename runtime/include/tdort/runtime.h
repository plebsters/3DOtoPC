// tdort/runtime.h - hybrid execution runtime linked into every converted game.
#pragma once
#include <cstdint>
#include <cstdio>
#include <functional>
#include <memory>
#include <string>
#include <unordered_map>
#include <unordered_set>
#include <vector>
#include "cpu.h"
#include "memory.h"

namespace tdort {

using BlockFn = void (*)(Cpu*);

// One recompiled basic block, emitted by the converter.
struct RecompEntry {
    uint32_t guest_addr;
    uint32_t byte_len;
    BlockFn  fn;
};

// Symbols provided by the generated code of each converted game.
extern "C" const RecompEntry tdort_recomp_table[];
extern "C" const uint32_t    tdort_recomp_count;
extern "C" const char        tdort_image_hash[];     // fnv1a64 hex of the LaunchMe image
extern "C" const char        tdort_game_name[];
extern "C" const uint32_t    tdort_image_base;

// Host side of an HLE call. Arguments in r0-r3 (+stack), result in r0.
using HleFn = std::function<void(Cpu*)>;

struct HleEntry { std::string name; HleFn fn; uint64_t calls = 0; };

struct Platform;   // platform.h
struct AifImage;   // aif.h
class  Portfolio;  // OS replacement (src/hle_os.h)
class  Gfx;        // gfx.h
class  Audio;      // audio.h

struct RuntimeConfig {
    std::string data_dir   = "data";          // extracted disc contents
    std::string launch_me  = "LaunchMe";      // executable path relative to data_dir
    std::string hints_out  = "interp_hits.txt";
    bool interp_only       = false;           // ignore recompiled code (debugging)
    bool trace             = false;
    uint32_t cpu_hz        = 12500000;        // ARM60 at 12.5 MHz
    uint32_t refresh_hz    = 60;
};

struct Stats {
    uint64_t recomp_blocks = 0, interp_blocks = 0, hle_calls = 0, invalidations = 0;
};

class Runtime {
public:
    explicit Runtime(RuntimeConfig cfg);
    ~Runtime();

    bool load_game(std::string& err);       // load LaunchMe, verify hash, set entry state
    int  run(Platform* platform);           // main loop; returns exit code
    void run_for(uint64_t cycles);          // execute ~cycles worth of guest code
    void request_vblank_wait() { vblank_wait_ = true; }

    // HLE registration
    void     register_swi(uint32_t number, std::string name, HleFn fn);
    uint32_t register_trap(std::string name, HleFn fn);   // returns guest address
    bool     handle_swi(uint32_t comment, uint32_t pc);
    void     handle_trap(uint32_t addr);

    // Guest-memory allocator for HLE-owned structures (folios, items...).
    uint32_t hle_alloc(uint32_t size, uint32_t align = 4);

    Cpu    cpu;
    Memory mem;
    RuntimeConfig cfg;
    Stats  stats;
    std::unique_ptr<Gfx>   gfx;
    std::unique_ptr<Audio> audio;
    std::unique_ptr<Portfolio> os;
    uint32_t pad_state = 0;   // buttons, see input.h
    uint32_t retry_pc = 0;    // where a blocked OS call resumes: the SWI, or the trap address

    void log(const char* fmt, ...);

private:
    void install_recompiled();
    bool block_valid(uint32_t idx) const;
    void interp_block();
    bool check_pc(uint32_t pc);                    // false = crashed (report written)
    bool repeat_call(uint32_t site, uint32_t a0, uint32_t a1);
    uint64_t recent_calls_[16] = {};
    uint32_t recent_pos_ = 0, frames_ = 0;
    void crash(const char* reason, uint32_t pc);

    std::vector<int32_t> lookup_;            // guest word index -> recomp table index, -1 = none
    std::unordered_map<uint32_t, HleEntry> swis_;
    std::vector<HleEntry> traps_;
    std::unordered_set<uint32_t> unknown_swis_, interp_hits_;
public:
    std::unordered_set<uint32_t> io_seen_;
private:
    uint32_t image_lo_ = 0, image_hi_ = 0;
    uint32_t hle_heap_ = 0, hle_heap_end_ = 0;
    bool recomp_enabled_ = true;
    bool vblank_wait_ = false;
    // crash detection
    uint32_t ring_[64] = {};
    uint32_t ring_pos_ = 0, zero_run_ = 0, entry_ = 0;
    bool entered_ = false;
public:
    bool crashed = false;
    std::string crash_message;
private:
    FILE* logf_ = nullptr;
};



} // namespace tdort
