// runtime.cpp - hybrid dispatcher: recompiled blocks first, interpreter fallback.
#include "tdort/runtime.h"
#include "tdort/arm_exec.h"
#include "tdort/aif.h"
#include "tdort/gfx.h"
#include "tdort/audio.h"
#include "tdort/platform.h"
#include "hle_os.h"
#include <algorithm>
#include <cstdarg>
#include <fstream>
#include <iterator>

namespace tdort {

static Runtime* g_rt = nullptr;   // for the on_swi / on_undefined hooks
static constexpr uint64_t kHistoryCalls = 3000;  // OS calls always logged at start-up

bool on_swi(Cpu* c, uint32_t comment, uint32_t pc) { return c->rt->handle_swi(comment, pc); }

void on_undefined(Cpu* c, uint32_t insn, uint32_t pc) {
    c->rt->log("undefined/coprocessor instruction %08X at %08X (treated as NOP)", insn, pc);
}

Runtime::Runtime(RuntimeConfig config) : cfg(std::move(config)) {
    g_rt = this;
    cpu.mem = &mem;
    cpu.rt = this;
    gfx = std::make_unique<Gfx>(mem);
    audio = std::make_unique<Audio>(mem);
    logf_ = std::fopen("tdort.log", "w");
    // Hardware registers (Madam/Clio, ROM): log the first accesses to each
    // address so missing hardware shows up in the log, then read as 0.
    mem.io_user = this;
    mem.io_read = [](void* u, uint32_t addr, int size) -> uint32_t {
        auto* rt = static_cast<Runtime*>(u);
        if (rt->io_seen_.size() < 64 && rt->io_seen_.insert(addr).second)
            rt->log("hardware read %08X (size %d) at pc %08X: not emulated, returns 0", addr, size, rt->cpu.r[15]);
        return 0;
    };
    mem.io_write = [](void* u, uint32_t addr, uint32_t val, int size) {
        auto* rt = static_cast<Runtime*>(u);
        if (rt->io_seen_.size() < 64 && rt->io_seen_.insert(addr | 1).second)
            rt->log("hardware write %08X = %08X (size %d) at pc %08X: ignored", addr, val, size, rt->cpu.r[15]);
    };
    // OS structures live at the top of DRAM; the game's stack sits just below.
    hle_heap_ = kHleHeapBase;
    hle_heap_end_ = kDramSize;
}

Runtime::~Runtime() {
    if (!interp_hits_.empty()) {
        // Feed these back to the converter (--hints) to recompile them next time.
        if (FILE* f = std::fopen(cfg.hints_out.c_str(), "w")) {
            for (uint32_t a : interp_hits_) std::fprintf(f, "%08X\n", a);
            std::fclose(f);
        }
    }
    {   // the most-called OS functions: a spinning game shows up here
        std::vector<std::pair<uint64_t, std::string>> top;
        for (auto& kv : swis_) if (kv.second.calls) top.push_back({kv.second.calls, kv.second.name});
        for (auto& t : traps_) if (t.calls) top.push_back({t.calls, t.name});
        std::sort(top.rbegin(), top.rend());
        std::string line;
        for (size_t i = 0; i < top.size() && i < 8; ++i) line += " " + top[i].second + "=" + std::to_string(top[i].first);
        log("most called:%s", line.c_str());
    }
    log("stats: recomp=%llu interp=%llu hle=%llu invalidations=%llu",
        (unsigned long long)stats.recomp_blocks, (unsigned long long)stats.interp_blocks,
        (unsigned long long)stats.hle_calls, (unsigned long long)stats.invalidations);
    if (logf_) std::fclose(logf_);
    if (g_rt == this) g_rt = nullptr;
}

void Runtime::log(const char* fmt, ...) {
    char small[1024];
    std::vector<char> big;
    const char* buf = small;
    va_list ap; va_start(ap, fmt);
    va_list ap2; va_copy(ap2, ap);
    const int n = std::vsnprintf(small, sizeof small, fmt, ap);
    va_end(ap);
    if (n >= int(sizeof small)) {          // long lines (task status) are written whole
        big.resize(size_t(n) + 1);
        std::vsnprintf(big.data(), big.size(), fmt, ap2);
        buf = big.data();
    }
    va_end(ap2);
    {   // remember problems for the diagnosis report
        static const char* keys[] = {"not found", "out of memory", "unimplemented", "not supported", "failed", "HLE stub", "refused"};
        for (const char* k : keys) {
            if (!std::strstr(buf, k)) continue;
            std::string key(buf);
            if (key.size() > 160) key.resize(160);
            auto it = problems.find(key);
            if (it != problems.end()) ++it->second.first;
            else if (problems.size() < 60) problems[key] = {1u, frames_};
            break;
        }
    }
    // A game polling in a loop can repeat one line millions of times: show the
    // first few, then only a running count.
    static std::string last;
    static uint64_t repeats = 0;
    auto emit = [this](const char* line) {
        std::fprintf(stderr, "[tdort] %s\n", line);
        if (logf_) { std::fprintf(logf_, "%s\n", line); std::fflush(logf_); }
    };
    if (last == buf) {
        ++repeats;
        if (repeats <= 3) emit(buf);
        else if ((repeats & (repeats - 1)) == 0) {   // 4, 8, 16, ... 
            char note[96];
            std::snprintf(note, sizeof note, "  (previous line repeated %llu times so far)", (unsigned long long)repeats);
            emit(note);
        }
        return;
    }
    if (repeats > 3) {
        char note[96];
        std::snprintf(note, sizeof note, "  (previous line repeated %llu times in total)", (unsigned long long)repeats);
        emit(note);
    }
    last = buf; repeats = 0;
    emit(buf);
}

bool Runtime::load_game(std::string& err) {
    std::string path = cfg.data_dir + "/" + cfg.launch_me;
    std::ifstream f(path, std::ios::binary);
    if (!f) { err = "cannot open " + path; return false; }
    std::vector<uint8_t> file((std::istreambuf_iterator<char>(f)), std::istreambuf_iterator<char>());
    AifImage img;
    if (!parse_aif(file, img, err)) return false;
    if (img.end() > kStackTop - kStackSize) { err = "image does not fit in DRAM"; return false; }

    mem.write_block(img.image_base, img.bytes.data(), img.load_size());
    entry_ = img.entry;
    {
        auto w = [&](uint32_t off) { return load_be32(&img.bytes[off]); };
        log("AIF: file=%u ro=%u rw=%u debug=%u zi=%u mode=%u%s%s%s", img.file_size, img.ro_size, img.rw_size,
            img.debug_size, img.zi_size, img.address_mode, img.compressed ? " compressed" : "",
            img.self_relocating ? " self-relocating" : "", img.zero_init_code ? " zero-init-code" : "");
        log("AIF header: %08X %08X %08X %08X %08X | +40: %08X", w(0), w(4), w(8), w(12), w(16), w(0x40));
        if (img.load_size() > 0x100)
            log("3DO header (+80): %08X %08X %08X %08X %08X %08X %08X %08X", w(0x80), w(0x84), w(0x88), w(0x8C),
                w(0x90), w(0x94), w(0x98), w(0x9C));
    }
    image_lo_ = img.image_base;
    image_hi_ = img.image_base + img.load_size();

    const std::string hash = fnv1a64_hex(img.bytes);
    recomp_enabled_ = !cfg.interp_only;
    if (recomp_enabled_ && hash != tdort_image_hash) {
        log("LaunchMe hash %s does not match recompiled code (%s): interpreter only",
            hash.c_str(), tdort_image_hash);
        recomp_enabled_ = false;
    }
    if (recomp_enabled_) install_recompiled();

    os = std::make_unique<Portfolio>(*this, img);   // sets r5/r6/r7, sp, sl
    if (const char* dump = std::getenv("TDORT_DUMP_API")) dump_api(dump);

    // The AIF zero-init routine starts with a "debug initialisation" instruction
    // at +0x40; when it is a SWI it only notifies a debugger, so it must not
    // change any register.
    const uint32_t dbg_insn = load_be32(&img.bytes[0x40]);
    if ((dbg_insn & 0x0F000000u) == 0x0F000000u && !swis_.count(dbg_insn & 0xFFFFFF))
        register_swi(dbg_insn & 0xFFFFFF, "AIF debug init (ignored)", [](Cpu*) {});

    // The same debug-init SWI (0x10A) heads programs a game loads later.
    if (!swis_.count(0x10A)) register_swi(0x10A, "AIF debug init (ignored)", [](Cpu*) {});
    // SWIs 0x100-0x1FF belong to the debugger folio. On a retail console it
    // isn't installed and the OS hands them to the ROM, which ignores them and
    // leaves the registers alone (Madden uses 0x103/0x104 around its cel drawing).
    for (uint32_t n = 0x100; n < 0x200; ++n)
        if (!swis_.count(n)) register_swi(n, "debugger SWI (ignored)", [](Cpu*) {});
    // SWI 0x10: the C library's start-up asks for its environment (command
    // line, memory limit); answering 0 lets it use its defaults.
    if (!swis_.count(0x10)) register_swi(0x10, "GetEnv (defaults)", [this](Cpu*) { cpu.r[0] = 0; });

    // The AIF header word at +0x10 is the program-exit instruction (a SWI).
    const uint32_t exit_insn = load_be32(&img.bytes[0x10]);
    if ((exit_insn & 0x0F000000u) == 0x0F000000u && !swis_.count(exit_insn & 0xFFFFFF))
        register_swi(exit_insn & 0xFFFFFF, "AIF exit", [this](Cpu*) { os->exit_current("AIF exit"); });

    // Entry state: start at the AIF header so its own init code runs.
    cpu.r[14] = register_trap("exit", [](Cpu* c) { c->halted = true; });
    cpu.r[15] = img.image_base;
    log("loaded %s: base=%08X entry=%08X size=%u zi=%u recompiled=%s blocks=%u",
        path.c_str(), img.image_base, img.entry, img.load_size(), img.zi_size,
        recomp_enabled_ ? "yes" : "no", recomp_enabled_ ? tdort_recomp_count : 0u);
    return true;
}

void Runtime::install_recompiled() {
    lookup_.assign(kRamEnd / 4, -1);
    for (uint32_t i = 0; i < tdort_recomp_count; ++i) {
        const RecompEntry& e = tdort_recomp_table[i];
        if (e.guest_addr >= kRamEnd || (e.guest_addr & 3)) continue;
        lookup_[e.guest_addr >> 2] = int32_t(i);
        for (uint32_t a = e.guest_addr; a < e.guest_addr + e.byte_len; a += 1u << kGranuleShift)
            mem.code_granule[a >> kGranuleShift] = 1;
        mem.code_granule[(e.guest_addr + e.byte_len - 1) >> kGranuleShift] = 1;
    }
}

bool Runtime::block_valid(uint32_t idx) const {
    const RecompEntry& e = tdort_recomp_table[idx];
    uint32_t g0 = e.guest_addr >> kGranuleShift, g1 = (e.guest_addr + e.byte_len - 1) >> kGranuleShift;
    for (uint32_t g = g0; g <= g1; ++g) if (mem.granule_dirty[g]) return false;
    return true;
}

// Interpret one basic block (until control flow changes or 64 instructions).
void Runtime::interp_block() {
    uint32_t pc = cpu.r[15];
    if (pc >= image_lo_ && pc < image_hi_) interp_hits_.insert(pc);
    for (int n = 0; n < 64 && !cpu.halted; ++n) {
        uint32_t insn = mem_r32(&cpu, pc);
        // A run of all-zero words is almost never real code: the game jumped
        // into memory that was never loaded (or was freed).
        if (insn == 0) {
            if (++zero_run_ >= 16) { crash("executing zeroed memory (jumped to unloaded or freed memory)", pc); return; }
        } else zero_run_ = 0;
        cpu.cycles += 1;
        if (exec(&cpu, insn, pc)) return;
        pc += 4;
        cpu.r[15] = pc;
        if (pc >= kTrapBase && pc < kTrapBase + kTrapSize) return;
    }
}

void Runtime::run_for(uint64_t budget) {
    const uint64_t stop = cpu.cycles + budget;
    while (cpu.cycles < stop && !cpu.halted && !vblank_wait_) {
        const uint32_t pc = cpu.r[15];
        if (pc >= kTrapBase && pc < kTrapBase + kTrapSize) { handle_trap(pc); continue; }
        ring_[ring_pos_++ & 63] = pc;
        if (os && os->mt) os->preempt();     // may switch to another task
        if (cpu.r[15] != pc) continue;
        if (!check_pc(pc)) break;
        if (recomp_enabled_ && pc < kRamEnd) {
            int32_t idx = lookup_[pc >> 2];
            if (idx >= 0) {
                if (block_valid(uint32_t(idx))) {
                    tdort_recomp_table[idx].fn(&cpu);
                    ++stats.recomp_blocks;
                    continue;
                }
                lookup_[pc >> 2] = -1;   // overwritten: fall back permanently
                ++stats.invalidations;
                log("recompiled block %08X invalidated by a write; interpreting", pc);
            }
        }
        interp_block();
        ++stats.interp_blocks;
    }
    vblank_wait_ = false;
}

// Catches jumps to places code can never legitimately be, before the CPU
// slides through memory and the evidence is gone.
bool Runtime::check_pc(uint32_t pc) {
    if (!entered_ && pc == entry_) entered_ = true;
    if (entered_ && pc >= image_lo_ && pc < image_lo_ + 0x80 && pc != image_lo_ + 0x10 /* AIF exit instruction */)
        { crash("jump into the AIF header (likely a call through a NULL or uninitialised function pointer)", pc); return false; }
    if (pc >= kStackTop - kStackSize && pc < kDramSize && !(pc >= hle_code_lo_ && pc < hle_code_hi_))
        { crash("jump into the stack / OS data area", pc); return false; }
    if (pc >= kRamEnd)
        { crash("jump outside RAM", pc); return false; }
    return true;
}

void Runtime::crash(const char* reason, uint32_t pc) {
    if (crashed) return;
    crashed = true;
    cpu.halted = true;
    log("==================== CRASH ====================");
    log("reason: %s", reason);
    { char b[200]; std::snprintf(b, sizeof b, "%s at %08X", reason, pc); crash_reason = b; }
    log("pc=%08X  cpsr=%08X  cycles=%llu", pc, cpu.cpsr(), (unsigned long long)cpu.cycles);
    for (int i = 0; i < 16; i += 4)
        log("r%-2d=%08X  r%-2d=%08X  r%-2d=%08X  r%-2d=%08X", i, cpu.r[i], i + 1, cpu.r[i + 1],
            i + 2, cpu.r[i + 2], i + 3, cpu.r[i + 3]);
    // Last blocks executed, oldest first. The second-to-last one made the bad jump.
    std::string trail;
    uint32_t prev_good = 0;
    for (uint32_t k = 0; k < 64; ++k) {
        uint32_t a = ring_[(ring_pos_ + k) & 63];
        if (!a && trail.empty()) continue;
        char b[12]; std::snprintf(b, sizeof b, "%08X ", a);
        trail += b;
        if (k < 63 && a >= image_lo_ && a < image_hi_) prev_good = a;
    }
    log("last blocks (oldest first): %s", trail.c_str());
    if (prev_good) {
        log("code at the last block inside the game image (%08X):", prev_good);
        for (uint32_t a = prev_good; a < prev_good + 64 && a < kRamEnd - 3; a += 16)
            log("  %08X: %08X %08X %08X %08X", a, mem_r32(&cpu, a), mem_r32(&cpu, a + 4),
                mem_r32(&cpu, a + 8), mem_r32(&cpu, a + 12));
    }
    log("stack (sp=%08X):", cpu.r[13]);
    for (uint32_t a = cpu.r[13] & ~3u, n = 0; n < 4 && a < kRamEnd - 15; ++n, a += 16)
        log("  %08X: %08X %08X %08X %08X", a, mem_r32(&cpu, a), mem_r32(&cpu, a + 4),
            mem_r32(&cpu, a + 8), mem_r32(&cpu, a + 12));
    log("===============================================");
    char msg[256];
    std::snprintf(msg, sizeof msg, "The game stopped: %s at %08X.\n\nDetails are in tdort.log.", reason, pc);
    crash_message = msg;
}

int Runtime::run(Platform* platform) {
    // Instructions per frame: the ARM60 runs 12.5 MHz from DRAM with no cache,
    // so an instruction takes about cfg.cpi cycles on average.
    static const double cpi = [this] { const char* e = std::getenv("TDORT_CPI"); return e ? std::max(0.25, std::atof(e)) : cfg.cpi; }();
    const uint64_t per_frame = uint64_t(double(cfg.cpu_hz) / cfg.refresh_hz / cpi);
    uint32_t busy_frames = 0;
    static const bool speed_trace = std::getenv("TDORT_SPEED_TRACE") != nullptr;
    std::vector<int16_t> mixbuf;
    const int audio_frames = Audio::kRate / int(cfg.refresh_hz);
    mixbuf.resize(size_t(audio_frames) * 2);
    while (!cpu.halted) {
        if (!platform->pump()) break;
        pad_state = platform->pad();
        {   // F2: next disc (for games that notice a disc change some other way)
            static uint32_t prev = 0;
            if ((pad_state & PadNextDisc) && !(prev & PadNextDisc) && os) os->next_disc();
            prev = pad_state;
        }
        if (os) os->frame(pad_state);
        if (os && ++frames_ % 120 == 0 && (frames_ <= 120 * 30 || std::getenv("TDORT_AUDIO_STATUS")))
            log("%s", os->status_line(frames_).c_str());
        const uint64_t frame_start = cpu.cycles;
        run_for(per_frame);
        // Time passes while the game waits for the vertical blank, just as the
        // real clock keeps running while the CPU idles. Without this the clock
        // (timer device, SampleSystemTime) only counted executed instructions,
        // so a game that mostly waits saw seconds take minutes: Jurassic Park
        // Interactive held its "3 seconds" copyright screen forever.
        if (cpu.cycles < frame_start + per_frame) cpu.cycles = frame_start + per_frame;
        else ++busy_frames;          // the game used the whole frame: it runs as fast as the CPU allows
        if (speed_trace && frames_ % 120 == 0) { log("speed: %u of the last 120 frames used all the CPU", busy_frames); busy_frames = 0; }
        // Debugging aid: TDORT_DUMPMEM=frame:addr:len writes guest memory to mem_<frame>.bin.
        if (const char* dm = std::getenv("TDORT_DUMPMEM")) {
            unsigned f = 0, a = 0, l = 0;
            if (std::sscanf(dm, "%u:%x:%x", &f, &a, &l) == 3 && f == frames_ && a + uint64_t(l) <= mem.ram.size()) {
                char name[64]; std::snprintf(name, sizeof name, "mem_%u.bin", f);
                if (FILE* fp = std::fopen(name, "wb")) { std::fwrite(&mem.ram[a], 1, l, fp); std::fclose(fp); }
            }
        }
        const uint32_t* pixels = gfx->scan_out();
        if (frames_ % 10 == 0) {     // when the picture last changed, for the diagnosis report
            uint64_t h = 1469598103934665603ull;
            for (int i = 0; i < Gfx::kWidth * Gfx::kHeight; i += 7) h = (h ^ pixels[i]) * 1099511628211ull;
            if (h != screen_hash_) { screen_hash_ = h; screen_change_frame_ = frames_; }
        }
        platform->present(pixels, Gfx::kWidth, Gfx::kHeight);
        audio->mix(mixbuf.data(), audio_frames);
        platform->queue_audio(mixbuf.data(), audio_frames);
        platform->wait_frame(cfg.refresh_hz);
    }
    if (os) {
        const std::string verdict = os->write_report(crashed ? "crashed" : cpu.halted ? "exited" : "stopped", screen_change_frame_);
        log("diagnosis: %s", verdict.c_str());
    }
    if (crashed) { platform->fatal(crash_message.c_str()); return 3; }
    log("guest %s, r0=%u", cpu.halted ? "exited" : "stopped", cpu.r[0]);
    return int(cpu.r[0]);
}

// ---- HLE plumbing ---------------------------------------------------------

void Runtime::register_swi(uint32_t number, std::string name, HleFn fn, bool stub) {
    swis_[number] = HleEntry{std::move(name), std::move(fn), 0, stub};
}

uint32_t Runtime::register_trap(std::string name, HleFn fn, bool stub) {
    traps_.push_back(HleEntry{std::move(name), std::move(fn), 0, stub});
    return kTrapBase + uint32_t(traps_.size() - 1) * 4;
}

// TDORT_DUMP_API=file: the OS calls this runtime provides, for checking
// against the 3DO's own tables (tools/api_coverage.py).
void Runtime::dump_api(const char* path) const {
    FILE* f = std::fopen(path, "w");
    if (!f) return;
    for (const auto& t : traps_) std::fprintf(f, "vector\t%s\t%s\n", t.name.c_str(), t.stub ? "stub" : "impl");
    for (const auto& [n, e] : swis_) std::fprintf(f, "swi\t%06X\t%s\t%s\n", n, e.name.c_str(), e.stub ? "stub" : "impl");
    std::fclose(f);
}

// True if this exact call (site and first two arguments) was logged recently:
// retried waits would otherwise flood the history.
bool Runtime::repeat_call(uint32_t site, uint32_t a0, uint32_t a1) {
    static const bool all = std::getenv("TDORT_TRACE_ALL") != nullptr;   // log every call, repeats included
    if (all) return false;
    const uint64_t key = (uint64_t(site) << 32) ^ (uint64_t(a0) * 2654435761u) ^ a1;
    for (uint64_t k : recent_calls_) if (k == key) return true;
    recent_calls_[recent_pos_++ % 16] = key;
    return false;
}

// TDORT_WATCH=<hex address>: logs when that word changes, checked at each OS
// call (so the change happened since the previous call).
static void watch_check(Runtime& rt, const char* what) {
    static const uint32_t addr = [] { const char* w = std::getenv("TDORT_WATCH"); return w ? uint32_t(std::strtoul(w, nullptr, 16)) : 0u; }();
    static uint32_t last = 0; static bool init = false;
    if (!addr || addr + 4 > kRamEnd) return;
    const uint32_t v = load_be32(&rt.mem.ram[addr]);
    if (!init) { init = true; last = v; return; }
    if (v != last) { rt.log("watch %08X: %08X -> %08X before %s (pc=%08X lr=%08X)", addr, last, v, what, rt.cpu.r[15], rt.cpu.r[14]); last = v; }
}

bool Runtime::handle_swi(uint32_t comment, uint32_t pc) {
    ++stats.hle_calls;
    { char w[32]; std::snprintf(w, sizeof w, "SWI %X", comment); watch_check(*this, w); }
    auto it = swis_.find(comment);
    if (it == swis_.end()) {
        if (unknown_swis_.insert(comment).second)
            log("unimplemented SWI 0x%06X at %08X (r0=%08X r1=%08X) -> returning 0",
                comment, pc, cpu.r[0], cpu.r[1]);
        cpu.r[0] = 0;
        return false;
    }
    ++it->second.calls;
    if ((cfg.trace || stats.hle_calls <= kHistoryCalls) && !repeat_call(pc, cpu.r[0], cpu.r[1]))
        log("call %llu: %s(%08X, %08X, %08X, %08X) at %08X", (unsigned long long)stats.hle_calls,
            it->second.name.c_str(), cpu.r[0], cpu.r[1], cpu.r[2], cpu.r[3], pc);
    if (os) os->note_call(it->second.name.c_str(), cpu.r, pc);
    cpu.r[15] = pc + 4;          // handlers may redirect by writing r15
    retry_pc = pc;
    it->second.fn(&cpu);
    return cpu.halted || cpu.r[15] != pc + 4;
}

void Runtime::handle_trap(uint32_t addr) {
    ++stats.hle_calls;
    uint32_t idx = (addr - kTrapBase) / 4;
    watch_check(*this, idx < traps_.size() ? traps_[idx].name.c_str() : "trap");
    const uint32_t ret = cpu.r[14];
    cpu.r[15] = ret;             // default: behave like a function that returns
    retry_pc = addr;             // a blocked vector call re-enters the trap (lr unchanged)
    if (idx < traps_.size()) {
        ++traps_[idx].calls;
        if (os) os->note_call(traps_[idx].name.c_str(), cpu.r, ret - 4);
        if ((cfg.trace || stats.hle_calls <= kHistoryCalls) && !repeat_call(ret - 4, cpu.r[0], cpu.r[1]))
            log("call %llu: %s(%08X, %08X, %08X, %08X) from %08X", (unsigned long long)stats.hle_calls,
                traps_[idx].name.c_str(), cpu.r[0], cpu.r[1], cpu.r[2], cpu.r[3], ret - 4);
        traps_[idx].fn(&cpu);
    } else {
        log("jump to unregistered trap %08X; halting", addr);
        cpu.halted = true;
    }
}

uint32_t Runtime::hle_alloc(uint32_t size, uint32_t align) {
    uint32_t a = (hle_heap_ + align - 1) & ~(align - 1);
    if (a + size > hle_heap_end_) { log("HLE heap exhausted"); return 0; }
    hle_heap_ = a + size;
    std::memset(&mem.ram[a], 0, size);
    return a;
}

uint32_t Runtime::hle_code(const uint32_t* words, size_t count) {
    const uint32_t a = hle_alloc(uint32_t(count * 4));
    if (!a) return 0;
    for (size_t i = 0; i < count; ++i) {
        mem.ram[a + i * 4] = uint8_t(words[i] >> 24); mem.ram[a + i * 4 + 1] = uint8_t(words[i] >> 16);
        mem.ram[a + i * 4 + 2] = uint8_t(words[i] >> 8); mem.ram[a + i * 4 + 3] = uint8_t(words[i]);
    }
    if (!hle_code_lo_) hle_code_lo_ = a;
    hle_code_lo_ = std::min(hle_code_lo_, a);
    hle_code_hi_ = std::max(hle_code_hi_, a + uint32_t(count * 4));
    return a;
}

} // namespace tdort
