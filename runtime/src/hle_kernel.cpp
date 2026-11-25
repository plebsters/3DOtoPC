// hle_kernel.cpp - Portfolio kernel replacement: items, memory, lists, signals,
// messages, IO dispatch, and the KernelBase vector table.
#include "hle_os.h"
#include "tdort/gfx.h"
#include <algorithm>
#include <cctype>
#include <cstring>
#include <filesystem>

namespace tdort {

namespace fs = std::filesystem;

using namespace abi;

static std::string lower(std::string s) {
    for (auto& ch : s) ch = char(std::tolower((unsigned char)ch));
    return s;
}

// ============================================================================
// construction
// ============================================================================

Portfolio::Portfolio(Runtime& r, const AifImage& img) : rt(r), c(r.cpu) {
    items_.emplace_back();      // item 0 is never valid

    // Game heap: everything between the image's end and the stack, plus VRAM.
    uint32_t lo = (img.end() + 15) & ~15u;
    uint32_t hi = kStackTop - kStackSize;
    if (lo < hi) free_dram_[lo] = hi - lo;
    free_vram_[kVramBase] = kVramSize;

    install_kernel_vectors();
    install_kernel_swis();
    install_file();
    install_graphics();
    install_other_folios();
    install_input();
    install_tasks();

    // On the console the OS has made dozens of items (folios, devices,
    // drivers, its own tasks, ports and messages) before a game starts, so a
    // game's first items have numbers well above 24. Quarterback Attack's
    // buffer thread tells its commands (small messages carrying 1..24) from
    // I/O completions (carrying the IOReq's item) by that number; its IOReq
    // was item 24 here and was taken for a command. Game items now start at
    // 64. TDORT_OLD_ITEM_NUMBERS=1: number them right after the runtime's.
    if (!std::getenv("TDORT_OLD_ITEM_NUMBERS"))
        while (items_.size() < 64) { items_.emplace_back(); items_.back().alive = false; }

    // Task start-up state, matching what the SDK's cstartup expects.
    const uint32_t argv0 = guest_string("LaunchMe");
    const uint32_t argv = rt.hle_alloc(8);
    w32(argv, argv0); w32(argv + 4, 0);
    c.r[kRegArgc] = 1;
    // The OS also leaves the argument count in r4 when a program starts.
    // Alone in the Dark reads r4, never set by its own startup code, as its
    // language: 1 is English, the only text pack on the US disc (0 asked for
    // French, missing, and the game unpacked garbage).
    c.r[4] = 1;
    c.r[kRegArgv] = argv;
    c.r[kRegKernelBase] = kbase_;
    c.r[13] = kStackTop - 16;
    c.r[10] = kStackTop - kStackSize + 512;     // APCS stack limit (sl), checked by every function prologue
    rt.log("HLE: KernelBase=%08X task=%08X heap=%08X-%08X", kbase_, tasks_.empty() ? 0u : tasks_[0].addr, lo, hi);
}

Portfolio::~Portfolio() {
    for (auto& it : items_) if (it.fp) std::fclose(it.fp);
}

// ============================================================================
// guest memory helpers
// ============================================================================

uint32_t Portfolio::r32(uint32_t a) const { return a < kRamEnd - 3 ? load_be32(&rt.mem.ram[a]) : 0; }
void Portfolio::w32(uint32_t a, uint32_t v) { if (a < kRamEnd - 3) { store_be32(&rt.mem.ram[a], v); rt.mem.invalidate(a, 4); } }
uint8_t Portfolio::r8(uint32_t a) const { return a < kRamEnd ? rt.mem.ram[a] : 0; }
void Portfolio::w8(uint32_t a, uint8_t v) { if (a < kRamEnd) { rt.mem.ram[a] = v; rt.mem.invalidate(a, 1); } }

bool Portfolio::guest_range_ok(uint32_t a, uint32_t len) const {
    return a < kRamEnd && len <= kRamEnd && a + len <= kRamEnd;
}

std::string Portfolio::rstr(uint32_t a, size_t max) const {
    std::string s;
    if (!a) return s;
    for (size_t i = 0; i < max && a + i < kRamEnd; ++i) {
        char ch = char(rt.mem.ram[a + i]);
        if (!ch) break;
        s += ch;
    }
    return s;
}

uint32_t Portfolio::guest_string(const std::string& s) {
    uint32_t p = rt.hle_alloc(uint32_t(s.size() + 1));
    if (p) rt.mem.write_block(p, s.c_str(), uint32_t(s.size() + 1));
    return p;
}

uint32_t Portfolio::arg(int n) const {
    if (n < 4) return c.r[n];
    return r32(c.r[13] + 4 * uint32_t(n - 4));
}

bool Portfolio::tag_value(uint32_t tags, uint32_t tag, uint32_t& out) const {
    bool found = false;
    each_tag(tags, [&](uint32_t t, uint32_t a) { if (t == tag && !found) { out = a; found = true; } });
    return found;
}

void Portfolio::stub(const std::string& name, uint32_t result) {
    int& n = stub_counts_[name];
    if (n++ < 3)
        rt.log("HLE stub %s(%08X, %08X, %08X, %08X) -> %08X%s", name.c_str(), c.r[0], c.r[1], c.r[2], c.r[3],
               result, n == 3 ? " (further calls not logged)" : "");
    c.r[0] = result;
}

uint64_t Portfolio::usec() const { return c.cycles * 1000000ull / rt.cfg.cpu_hz; }

// ============================================================================
// items
// ============================================================================

HleItem& Portfolio::new_item(ItemKind k, uint8_t subsys, uint8_t type, uint32_t struct_size,
                             const std::string& name, bool from_game_heap) {
    HleItem it;
    it.num = int32_t(items_.size());
    it.kind = k; it.subsys = subsys; it.type = type; it.name = name;
    it.size = std::max<uint32_t>(struct_size, node::ItemNodeSize);
    // Item structures come from the game's memory, as on the console; when
    // that is full they go in the runtime's own area instead (no failure to
    // report: Road Rash fills its memory and keeps creating items).
    quiet_alloc_ = true;
    it.addr = from_game_heap ? alloc(it.size, kMemFromTop) : rt.hle_alloc(it.size);
    quiet_alloc_ = false;
    if (!it.addr) it.addr = rt.hle_alloc(it.size);
    // Item structures are the system's, not the creating task's: a device made
    // on first lookup by a short-lived program (Gex's movie player opens "mac")
    // must outlive it, so its memory is never freed with that program's.
    alloc_owner_.erase(it.addr);
    it.heap_owned = from_game_heap && it.addr && it.addr < kHleHeapBase;
    if (it.addr) {
        std::vector<uint8_t> zero(it.size, 0);
        rt.mem.write_block(it.addr, zero.data(), it.size);
        w8(it.addr + node::SubsysType, subsys);
        w8(it.addr + node::Type, type);
        w32(it.addr + node::Size, it.size);
        if (!name.empty()) w32(it.addr + node::Name, guest_string(name));
        w32(it.addr + node::Item, uint32_t(it.num));
        w32(it.addr + node::Owner, tasks_.empty() ? 0u : uint32_t(cur().item));
        it.owner_task = tasks_.empty() ? 0 : cur().item;
    }
    items_.push_back(std::move(it));
    return items_.back();
}

HleItem* Portfolio::item(int32_t num) {
    if (num <= 0 || size_t(num) >= items_.size() || !items_[size_t(num)].alive) return nullptr;
    return &items_[size_t(num)];
}

HleItem* Portfolio::find_item(uint32_t ctype, const std::string& name) {
    const uint8_t subsys = uint8_t(ctype >> 8), type = uint8_t(ctype);
    // intlOpenLocale(NULL) and similar: there is one locale, whatever name is asked for.
    if (subsys == 9 && type == 1 && locale_item_) return item(locale_item_);
    const std::string want = lower(name);
    for (auto& it : items_)
        if (it.alive && it.num && it.subsys == subsys && it.type == type && lower(it.name) == want) return &it;
    return nullptr;
}

// DeleteItem as the game calls it: a task (or thread) may delete only items
// it owns, and itself; anything else fails with a privilege error and the
// item stays. Sailor Moon S's music thread "unloads" the fight's mixer, which
// the main task made: on the console that fails, and the main task goes on
// using the mixer (deleting it silenced the fight). TDORT_OLD_DELETE_ANY=1:
// any task may delete any item, as before.
int32_t Portfolio::game_delete_item(int32_t num) {
    static const bool any = std::getenv("TDORT_OLD_DELETE_ANY") != nullptr;
    HleItem* it = item(num);
    if (!it) return kErrBadItem;
    if (!any && !tasks_.empty() && it->owner_task && it->owner_task != cur().item && num != cur().item) {
        if (rt.cfg.trace) rt.log("DeleteItem(%d): task %d does not own it (owner %d)", num, cur().item, it->owner_task);
        return kErrBadPriv;
    }
    return delete_item(num);
}

int32_t Portfolio::delete_item(int32_t num) {
    HleItem* it = item(num);
    if (!it) return kErrBadItem;
    static const bool log_items = std::getenv("TDORT_LOG_AUDIO_ITEMS") != nullptr;
    if (log_items) rt.log("DeleteItem(%d) by task %d", num, cur().item);
    if (it->kind == ItemKind::Folio) return kErrNotSupported;
    if (it->kind == ItemKind::Task) {
        HleTask* t = task_by_item(num);
        if (!t || t == &tasks_[0]) return kErrNotSupported;   // the main task ends via exit()
        task_exit(*t, "deleted");
        return 0;
    }
    // A screen group goes with its screens and bitmaps, and the frame buffers
    // the OS allocated for them return to VRAM. Snow Job makes a new group for
    // each scene and deletes the old one; with the buffers kept, VRAM ran out
    // after the intro. TDORT_OLD_SCREEN_DELETE=1: keep them.
    if ((it->kind == ItemKind::ScreenGroup || it->kind == ItemKind::Screen) && !std::getenv("TDORT_OLD_SCREEN_DELETE")) {
        const std::vector<int32_t> kids = it->children;
        it->children.clear();
        for (int32_t k : kids) delete_item(k);
        it = item(num);
        if (!it) return 0;
    }
    if (it->kind == ItemKind::Bitmap && it->owns_buffer && it->buffer) {
        free_mem(it->buffer);
        it->owns_buffer = false;
    }
    if (it->fp) { std::fclose(it->fp); it->fp = nullptr; }
    if (it->subsys == kSubsysAudio) audio_deleted(*it);
    if (it->heap_owned) free_mem(it->addr);
    it->alive = false;
    return 0;
}

// ============================================================================
// memory: first-fit over DRAM (then VRAM), 16-byte granularity
// ============================================================================

static uint32_t take(std::map<uint32_t, uint32_t>& fl, uint32_t size) {
    for (auto it = fl.begin(); it != fl.end(); ++it) {
        if (it->second < size) continue;
        uint32_t a = it->first, rest = it->second - size;
        fl.erase(it);
        if (rest) fl[a + size] = rest;
        return a;
    }
    return 0;
}

// Highest-address fit (MEMTYPE_FROMTOP): used for the OS's own structures so
// they never split the low, large blocks games allocate and free.
static uint32_t take_top(std::map<uint32_t, uint32_t>& fl, uint32_t size) {
    for (auto it = fl.rbegin(); it != fl.rend(); ++it) {
        if (it->second < size) continue;
        uint32_t base = it->first, len = it->second;
        uint32_t a = base + len - size;
        fl.erase(base);
        if (len > size) fl[base] = len - size;
        return a;
    }
    return 0;
}

// A fit whose start is a multiple of `align` (a power of two): the lowest
// one, or the highest with `from_top`. What is left either side stays free.
static uint32_t take_aligned(std::map<uint32_t, uint32_t>& fl, uint32_t size, uint32_t align, bool from_top) {
    auto fits = [&](uint32_t base, uint32_t len, uint32_t& a) {
        if (len < size) return false;
        if (from_top) a = (base + len - size) & ~(align - 1);
        else a = (base + align - 1) & ~(align - 1);
        return a >= base && uint64_t(a) + size <= uint64_t(base) + len;
    };
    auto use = [&](uint32_t base, uint32_t len, uint32_t a) {
        fl.erase(base);
        if (a > base) fl[base] = a - base;
        if (a + size < base + len) fl[a + size] = base + len - (a + size);
        return a;
    };
    uint32_t a = 0;
    if (from_top) {
        for (auto it = fl.rbegin(); it != fl.rend(); ++it) if (fits(it->first, it->second, a)) return use(it->first, it->second, a);
    } else {
        for (auto it = fl.begin(); it != fl.end(); ++it) if (fits(it->first, it->second, a)) return use(it->first, it->second, a);
    }
    return 0;
}

static void give(std::map<uint32_t, uint32_t>& fl, uint32_t a, uint32_t size) {
    auto it = fl.emplace(a, size).first;
    auto next = std::next(it);
    if (next != fl.end() && it->first + it->second == next->first) { it->second += next->second; fl.erase(next); }
    if (it != fl.begin()) {
        auto prev = std::prev(it);
        if (prev->first + prev->second == it->first) { prev->second += it->second; fl.erase(it); }
    }
}

// Takes the free parts of [lo, hi) out of a free list; the pieces taken are
// returned (start, size). Parts already in use are left alone.
static std::vector<std::pair<uint32_t, uint32_t>> take_range(std::map<uint32_t, uint32_t>& fl, uint32_t lo, uint32_t hi) {
    std::vector<std::pair<uint32_t, uint32_t>> got;
    if (lo >= hi) return got;
    auto it = fl.upper_bound(lo);
    if (it != fl.begin()) --it;
    while (it != fl.end() && it->first < hi) {
        const uint32_t a = it->first, e = it->first + it->second;
        if (e <= lo) { ++it; continue; }
        const uint32_t s0 = std::max(a, lo), s1 = std::min(e, hi);
        it = fl.erase(it);
        if (a < s0) fl.emplace(a, s0 - a);
        if (s1 < e) it = fl.emplace(s1, e - s1).first;
        got.emplace_back(s0, s1 - s0);
    }
    return got;
}

// Rebuilds each bank's free-page bitmap from the allocator's free ranges
// (a page counts as free when it lies wholly inside one). Doom sizes its
// memory pool from availMem(); with no free pages reported it quit with
// "Can't start memory manager".
void Portfolio::update_free_page_bits() {
    auto fill = [&](uint32_t mh, const std::map<uint32_t, uint32_t>& fl) {
        if (!mh) return;
        const uint32_t bits = r32(mh + memhdr::FreePageBits);
        const uint32_t words = r8(mh + memhdr::FreePageBitsSize);
        const uint32_t base = r32(mh + memhdr::MemBase);
        if (!bits || !words) return;
        std::vector<uint32_t> w(words, 0);
        for (const auto& [a, sz] : fl) {
            const uint32_t first = (a - base + 2047) / 2048, end = (a + sz - base) / 2048;
            for (uint32_t p = first; p < end && p < words * 32; ++p) w[p / 32] |= 1u << (p % 32);
        }
        // On the console the OS itself fills a few hundred KB of DRAM, out of
        // the game's sight; here its structures live elsewhere. Free pages at
        // the top of DRAM, up to a reserve, are left out of what games are
        // told is free (they stay allocatable). Road Rash takes all the
        // memory it is told about but 36 KB, then its track loader needed
        // more and every thread deadlocked on "Game Loading".
        if (mh == memhdr_dram_) {
            static const uint32_t reserve_pages = [] {
                const char* e = std::getenv("TDORT_OS_RESERVE_KB");
                return (e ? uint32_t(std::strtoul(e, nullptr, 10)) : 128u) / 2;
            }();
            uint32_t hidden = 0;
            for (int32_t p = int32_t(words * 32) - 1; p >= 0 && hidden < reserve_pages; --p)
                if (w[p / 32] & (1u << (p % 32))) { w[p / 32] &= ~(1u << (p % 32)); ++hidden; }
        }
        for (uint32_t i = 0; i < words; ++i) w32(bits + i * 4, w[i]);
    };
    fill(memhdr_dram_, free_dram_);
    fill(memhdr_vram_, free_vram_);
}

uint32_t Portfolio::alloc(uint32_t size, uint32_t flags) {
    if (size == 0 || size > kRamEnd) return 0;
    uint32_t sz = (size + 15) & ~15u;
    uint32_t a = 0;
    // MEMTYPE_STARTPAGE / MEMTYPE_SYSTEMPAGESIZE: the block starts on a 2 KB
    // page and (here) fills whole pages; MEMTYPE_INPAGE: it crosses no page.
    // Screen buffers are asked for this way, and games then work on whole
    // pages: Brain Dead 13 clears its background buffer page by page from the
    // page boundary, which wiped a thread stack that shared that page.
    constexpr uint32_t kPage = 2048, kStartPage = 0x02000000u, kSysPage = 0x04000000u, kInPage = 0x01000000u;
    uint32_t align = 16;
    if (flags & (kStartPage | kSysPage)) { align = kPage; sz = (sz + kPage - 1) & ~(kPage - 1); }
    else if ((flags & kInPage) && sz <= kPage) { align = 16; while (align < sz) align <<= 1; }
    // Small blocks come from the top of memory, large ones from the bottom, so
    // leftover small allocations (a thread stack a game never frees, message
    // buffers) can't split the space big buffers need. Slam 'n Jam needs one
    // 890 KB block each attract-mode cycle and leaks a 4 KB stack per cycle.
    static const bool split = std::getenv("TDORT_ALLOC_FLAT") == nullptr;
    const bool small = split && sz <= 8192;
    const bool top = (flags & kMemFromTop) || small;
    // Large blocks come from above the first 512 KB of DRAM when they fit
    // there: on the console the OS occupies low memory, so a game's big data
    // never starts near address 0. Theme Park relocates the offsets in a
    // sprite table only where they are below the data's address; its sprite
    // data at 0x1230 left most offsets unrelocated and drew garbage.
    // TDORT_OLD_ALLOC_LOW=1: lowest fit as before.
    static const bool old_low = std::getenv("TDORT_OLD_ALLOC_LOW") != nullptr;
    auto take_high_first = [&](std::map<uint32_t, uint32_t>& fl, uint32_t n) -> uint32_t {
        constexpr uint32_t kLowEnd = 0x80000;
        for (auto it = fl.begin(); it != fl.end(); ++it) {
            const uint32_t b = it->first, len = it->second;
            const uint32_t start = std::max(b, kLowEnd);
            if (start >= b + len || b + len - start < n) continue;
            fl.erase(it);
            if (start > b) fl[b] = start - b;
            if (start + n < b + len) fl[start + n] = b + len - (start + n);
            return start;
        }
        return take(fl, n);
    };
    auto pick = [&](std::map<uint32_t, uint32_t>& fl, uint32_t n) {
        if (align <= 16 && !top && !old_low && n >= 0x10000 && &fl == &free_dram_) return take_high_first(fl, n);
        return align > 16 ? take_aligned(fl, n, align, top) : top ? take_top(fl, n) : take(fl, n);
    };
    // The last 32 KB of DRAM is not given out in small pieces. Olympic Soccer
    // and Olympic Summer Games size a pool from AvailMem, then take whatever
    // is left in 52-byte blocks until AllocMem fails; with nothing left, the
    // SDK's CreateThread couldn't get a 1 KB stack and the game waited forever
    // for that thread. Requests of 256 bytes or more can still use it.
    // TDORT_SMALL_ALLOC_FLOOR_KB sets the size (0: off).
    static const uint32_t floor_bytes = [] {
        const char* e = std::getenv("TDORT_SMALL_ALLOC_FLOOR_KB");
        return (e ? uint32_t(std::strtoul(e, nullptr, 10)) : 32u) * 1024u;
    }();
    // A probe loop stops at its first failure, so one refusal is enough; after
    // it the floor is lifted and ordinary small requests can use that memory.
    static bool floor_tripped = false;
    auto dram_ok = [&]() {
        if (!floor_bytes || floor_tripped || sz >= 256) return true;
        uint32_t total = 0;
        for (const auto& [b, n] : free_dram_) total += n;
        if (total >= sz + floor_bytes) return true;
        floor_tripped = true;
        rt.log("AllocMem(%u): the last %u KB of DRAM are kept for larger requests (once)", size, floor_bytes / 1024);
        return false;
    };
    if (flags & kMemVram) a = pick(free_vram_, sz);
    else if (!(a = dram_ok() ? pick(free_dram_, sz) : 0) && !(flags & 0x00080000u /*MEMTYPE_DRAM*/))
        a = pick(free_vram_, sz);   // 3DO memory is unified enough (unless DRAM was asked for)
    if (!a) {
        if (!quiet_alloc_) {
            uint32_t total = 0, largest = 0;
            for (auto& [b, n] : free_dram_) { total += n; largest = std::max(largest, n); }
            rt.log("AllocMem(%u, %08X) failed: out of memory (DRAM free %u, largest block %u; lr=%08X)", size, flags, total, largest, c.r[14]);
        }
        return 0;
    }
    allocs_[a] = sz;
    update_free_page_bits();
    if (!tasks_.empty()) alloc_owner_[a] = owning_task(cur());
    static const char* mem_trace_env = std::getenv("TDORT_MEM_TRACE");
    static const bool mem_trace = mem_trace_env != nullptr;
    static const uint32_t mem_trace_min = mem_trace_env && std::string(mem_trace_env) == "all" ? 0u : 1024u;   // TDORT_MEM_TRACE=all: every size
    if (mem_trace && sz >= mem_trace_min)
        rt.log("alloc %u (%08X) -> %08X by \"%s\" lr=%08X", sz, flags, a, cur().name.c_str(), c.r[14]);
    std::vector<uint8_t> fill(sz, (flags & kMemFill) ? uint8_t(flags & kMemFillMask) : 0);
    rt.mem.write_block(a, fill.data(), sz);
    return a;
}

void Portfolio::free_mem(uint32_t a) {
    auto it = allocs_.find(a);
    if (it == allocs_.end()) { if (a) rt.log("FreeMem(%08X): not an allocation, ignored", a); return; }
    static const char* mem_trace_env = std::getenv("TDORT_MEM_TRACE");
    static const bool mem_trace = mem_trace_env != nullptr;
    static const uint32_t mem_trace_min = mem_trace_env && std::string(mem_trace_env) == "all" ? 0u : 1024u;
    if (mem_trace && it->second >= mem_trace_min) rt.log("free %08X (%u) by \"%s\" lr=%08X", a, it->second, cur().name.c_str(), c.r[14]);
    give(a >= kVramBase ? free_vram_ : free_dram_, it->first, it->second);
    allocs_.erase(it);
    alloc_owner_.erase(a);
    update_free_page_bits();
}

int32_t Portfolio::owning_task(const HleTask& t) {
    const uint32_t owner = t.addr ? r32(t.addr + task::ThreadTask) : 0;
    if (owner && owner != t.addr)
        for (const HleTask& o : tasks_) if (o.addr == owner) return o.item;
    return t.item;
}

// A task's memory goes back to the system when it ends, as on the 3DO.
// Gex plays each movie with a separate player program; its video buffers,
// left behind, used up video memory and the second movie's player failed.
void Portfolio::free_task_memory(int32_t owner) {
    std::vector<uint32_t> mine;
    for (auto& [a, o] : alloc_owner_) if (o == owner) mine.push_back(a);
    uint64_t bytes = 0;
    for (uint32_t a : mine) { auto it = allocs_.find(a); if (it != allocs_.end()) bytes += it->second; free_mem(a); }
    if (!mine.empty()) rt.log("freed %zu blocks (%llu bytes) owned by task %d", mine.size(), (unsigned long long)bytes, owner);
}

// ============================================================================
// lists: anchor at list+20 = {head, null, tailpred}
// ============================================================================

void Portfolio::list_init(uint32_t l, uint32_t name_ptr) {
    w8(l + node::SubsysType, kSubsysKernel);
    w8(l + node::Type, kNodeList);
    w32(l + node::Name, name_ptr);
    w32(l + list::Head, l + list::Null);
    w32(l + list::Null, 0);
    w32(l + list::TailPred, l + list::Head);
}
void Portfolio::list_add_head(uint32_t l, uint32_t n) {
    uint32_t first = r32(l + list::Head);
    w32(n + node::Next, first); w32(n + node::Prev, l + list::Head);
    w32(first + node::Prev, n); w32(l + list::Head, n);
}
void Portfolio::list_add_tail(uint32_t l, uint32_t n) {
    uint32_t last = r32(l + list::TailPred);
    w32(n + node::Next, l + list::Null); w32(n + node::Prev, last);
    w32(last + node::Next, n); w32(l + list::TailPred, n);
}
uint32_t Portfolio::list_rem_head(uint32_t l) {
    uint32_t first = r32(l + list::Head), next = r32(first + node::Next);
    if (!next) return 0;
    w32(l + list::Head, next); w32(next + node::Prev, l + list::Head);
    return first;
}
uint32_t Portfolio::list_rem_tail(uint32_t l) {
    uint32_t last = r32(l + list::TailPred), prev = r32(last + node::Prev);
    if (!prev) return 0;
    w32(l + list::TailPred, prev); w32(prev + node::Next, l + list::Null);
    return last;
}
void Portfolio::node_remove(uint32_t n) {
    uint32_t next = r32(n + node::Next), prev = r32(n + node::Prev);
    w32(prev + node::Next, next); w32(next + node::Prev, prev);
}
// Insert after the last node whose priority is >= n's (FIFO within a priority).
void Portfolio::list_insert_from_tail(uint32_t l, uint32_t n) {
    const uint8_t pri = r8(n + node::Priority);
    uint32_t p = r32(l + list::TailPred);
    while (r32(p + node::Prev) && r8(p + node::Priority) < pri) p = r32(p + node::Prev);
    uint32_t next = r32(p + node::Next);
    w32(n + node::Next, next); w32(n + node::Prev, p);
    w32(next + node::Prev, n); w32(p + node::Next, n);
}
// Insert before the first node whose priority is < n's.
void Portfolio::list_insert_from_head(uint32_t l, uint32_t n) {
    const uint8_t pri = r8(n + node::Priority);
    uint32_t p = r32(l + list::Head);
    while (r32(p + node::Next) && r8(p + node::Priority) >= pri) p = r32(p + node::Next);
    uint32_t prev = r32(p + node::Prev);
    w32(n + node::Next, p); w32(n + node::Prev, prev);
    w32(prev + node::Next, n); w32(p + node::Prev, n);
}
uint32_t Portfolio::list_find_named(uint32_t l, const std::string& name) {
    const std::string want = lower(name);
    int guard = 0;
    for (uint32_t n = r32(l + list::Head); r32(n + node::Next) && guard < 100000; n = r32(n + node::Next), ++guard)
        if (lower(rstr(r32(n + node::Name))) == want) return n;
    return 0;
}

// ============================================================================
// folios and vectors
// ============================================================================

void Portfolio::set_vector(uint32_t base, int32_t index, const std::string& name, HleFn fn) {
    uint32_t trap = rt.register_trap(name, std::move(fn));
    w32(base + uint32_t(index * 4), trap);
}

uint32_t Portfolio::make_folio(const std::string& name, uint8_t node_subsys, uint32_t nvec,
                               const std::map<int32_t, HleFn>& impl, int32_t* item_out) {
    // [ nvec vectors ][ Folio structure ]  ->  base points at the structure
    const uint32_t table = rt.hle_alloc(nvec * 4);
    HleItem& it = new_item(ItemKind::Folio, kSubsysKernel, kNodeFolio, folio::Size + 256, name, false);
    (void)node_subsys;
    const uint32_t base = it.addr;
    if (table + nvec * 4 != base) rt.log("HLE: folio %s vector table not adjacent (heap alignment)", name.c_str());
    w32(base + folio::OpenCount, 1);
    for (uint32_t i = 1; i <= nvec; ++i) {
        int32_t idx = -int32_t(i);
        auto f = impl.find(idx);
        std::string slot = name + "[" + std::to_string(idx) + "]";
        if (f != impl.end()) set_vector(base, idx, slot, f->second);
        else w32(base + uint32_t(idx * 4), rt.register_trap(slot, [this, slot](Cpu*) { stub(slot); }, true));
    }
    if (item_out) *item_out = it.num;
    rt.log("HLE folio %-10s base=%08X item=%d", name.c_str(), base, it.num);
    return base;
}

void Portfolio::install_kernel_vectors() {
    using M = std::map<int32_t, HleFn>;
    M v;
    v[kvRemHead]    = [this](Cpu*) { c.r[0] = list_rem_head(arg(0)); };
    v[kvAddHead]    = [this](Cpu*) { list_add_head(arg(0), arg(1)); };
    v[kvRemTail]    = [this](Cpu*) { c.r[0] = list_rem_tail(arg(0)); };
    v[kvAddTail]    = [this](Cpu*) { list_add_tail(arg(0), arg(1)); };
    v[kvInsertTail] = [this](Cpu*) { list_insert_from_tail(arg(0), arg(1)); };
    v[kvInsertHead] = [this](Cpu*) { list_insert_from_head(arg(0), arg(1)); };
    v[kvRemoveNode] = [this](Cpu*) { node_remove(arg(0)); };
    v[kvInitList]   = [this](Cpu*) { list_init(arg(0), arg(1)); };
    v[kvFindNamedNode] = [this](Cpu*) { c.r[0] = list_find_named(arg(0), rstr(arg(1))); };
    v[kvSetNodePri] = [this](Cpu*) { uint32_t n = arg(0); uint8_t old = r8(n + node::Priority);
                                     w8(n + node::Priority, uint8_t(arg(1))); c.r[0] = old; };
    // The vectors are the memory-list forms in every SDK version; the
    // AllocMem()/FreeMem() API is a wrapper that passes a free-memory list:
    //   -7 AllocMemFromMemLists(List *l, int32 size, uint32 flags)
    //   -8 FreeMemToMemLists(List *l, void *p, int32 size)
    // There is one heap here, so the list argument is accepted and ignored.
    v[kvAllocMem]   = [this](Cpu*) { c.r[0] = alloc(arg(1), arg(2)); };
    v[kvFreeMem]    = [this](Cpu*) { free_mem(arg(1)); };
    v[kvScavengeMem] = [this](Cpu*) { c.r[0] = 0; };
    v[kvFindImage]   = [this](Cpu*) { c.r[0] = 0; };          // FindImage: no ROM images here
    // -31: the C library's stack-limit check lands here (__rt_stkovf) when a
    // function needs more stack than the limit register (r10) allows; ip holds
    // the lowest address it needs. The kernel grows the stack into the memory
    // below and hands back a lower limit; every other register is kept.
    v[-31] = [this](Cpu*) {
        const uint32_t need = c.r[12];
        const uint32_t lim = (need > 0x1000 + 256 ? need - 256 : 0x1000) & ~15u;
        const uint32_t old_lim = c.r[10];
        if (lim < c.r[10]) c.r[10] = lim;
        // The memory the stack now reaches into must not be handed out: VR
        // Stalker's game program grew its stack 32 KB below its image, then
        // AllocMem gave it that memory and the zero fill wiped its own stack
        // (a jump to 0). The free parts are claimed for the task, and go back
        // when it ends. TDORT_OLD_STACK_GROW=1: don't claim.
        static const bool old_grow = std::getenv("TDORT_OLD_STACK_GROW") != nullptr;
        uint32_t claimed = 0;
        if (!old_grow && lim < old_lim && old_lim <= kRamEnd) {
            auto& fl = lim >= kVramBase ? free_vram_ : free_dram_;
            // 32 KB more below the new limit is kept for the next growth, so
            // small allocations don't settle right under a growing stack.
            const uint32_t lo = lim > 0x8000 + 0x1000 ? lim - 0x8000 : 0x1000;
            for (auto [a, n] : take_range(fl, lo, old_lim)) {
                allocs_[a] = n;
                alloc_owner_[a] = owning_task(cur());
                claimed += n;
            }
            if (claimed) update_free_page_bits();
        }
        static int logged = 0;
        if (logged++ < 3) rt.log("stack of task %s extended: limit now %08X (sp %08X), %u bytes claimed", cur().name.c_str(), c.r[10], c.r[13], claimed);
    };
    v[-17] = [](Cpu*) {};   // UniversalInsertNode: replaced by ARM code after the folio is made (below)
    // Memory lists: a program's private pools. Their memory comes from the
    // task's normal allocator here, so a MemList is just a small marker block.
    v[kvAllocMemML]   = [this](Cpu*) { c.r[0] = alloc(arg(1), arg(2)); };          // AllocMemFromMemList(ml, size, flags)
    v[kvFreeMemML]    = [this](Cpu*) { free_mem(arg(1)); };                       // FreeMemToMemList(ml, p, size)
    v[kvAllocMemList] = [this](Cpu*) { c.r[0] = alloc(64, kMemFill); };           // AllocMemList(p, name)
    v[kvFreeMemList]  = [this](Cpu*) { if (arg(0)) free_mem(arg(0)); };            // FreeMemList(ml)
    // CheckIO(ior): 1 once the request is done, 0 while in progress, <0 bad item.
    v[kvCheckIO] = [this](Cpu*) {
        HleItem* it = item(int32_t(arg(0)));
        if (!it || it->kind != ItemKind::IOReq) { c.r[0] = uint32_t(kErrBadItem); return; }
        c.r[0] = (r32(it->addr + ioreq::Flags) & kIoDone) ? 1 : 0;
    };
    // FindMH(void *p): the header of the memory bank containing p
    v[kvFindMH] = [this](Cpu*) {
        uint32_t p = arg(0);
        c.r[0] = (p >= kVramBase && p < kVramBase + kVramSize) ? memhdr_vram_ : (p < kDramSize ? memhdr_dram_ : 0);
    };
    v[kvGetPageSize] = [this](Cpu*) { c.r[0] = 2048; };
    v[kvGetMemAllocAlignment] = [this](Cpu*) { c.r[0] = 16; };
    v[kvGetMemTrackSize] = [this](Cpu*) { auto it = allocs_.find(arg(0)); c.r[0] = it == allocs_.end() ? 0 : it->second; };
    v[kvIsMemReadable] = [this](Cpu*) { c.r[0] = guest_range_ok(arg(0), arg(1)); };
    v[kvIsMemWritable] = [this](Cpu*) { c.r[0] = guest_range_ok(arg(0), arg(1)); };
    v[kvMemSet] = [this](Cpu*) {
        uint32_t d = arg(0), n = arg(2);
        if (guest_range_ok(d, n)) { std::memset(&rt.mem.ram[d], int(arg(1) & 0xFF), n); rt.mem.invalidate(d, n); }
        c.r[0] = d;
    };
    v[kvMemCpy] = [this](Cpu*) {
        uint32_t d = arg(0), s = arg(1), n = arg(2);
        if (guest_range_ok(d, n) && guest_range_ok(s, n)) { std::memmove(&rt.mem.ram[d], &rt.mem.ram[s], n); rt.mem.invalidate(d, n); }
        c.r[0] = d;
    };
    // Item 0 is the kernel itself: LookupItem(0) gives KernelBase (a folio
    // node). The 1.x WaitIO looks up io_MsgItem, which is 0 for a request
    // without a reply port, and checks the node type; FIFA's timer task
    // depends on it.
    v[kvLookupItem] = [this](Cpu*) {
        if (arg(0) == 0) { c.r[0] = kbase_; return; }
        HleItem* it = item(int32_t(arg(0)));
        c.r[0] = it ? it->addr : 0;
    };
    v[kvCheckItem]  = [this](Cpu*) {
        HleItem* it = item(int32_t(arg(0)));
        c.r[0] = (it && it->subsys == uint8_t(arg(1)) && it->type == uint8_t(arg(2))) ? it->addr : 0;
    };
    v[kvItemOpened] = [this](Cpu*) { c.r[0] = item(int32_t(arg(1))) ? 0 : uint32_t(kErrBadItem); };
    v[kvFindTagArg] = [this](Cpu*) {
        uint32_t found = 0, want = arg(1);
        for (uint32_t t = arg(0), guard = 0; t && guard < 4096; ++guard) {
            uint32_t tag = r32(t);
            if (tag == kTagEnd) break;
            if (tag == kTagJump) { t = r32(t + 4); continue; }
            if (tag == want) { found = t; break; }
            t += 8;
        }
        c.r[0] = found;
    };
    v[kvNextTagArg] = [this](Cpu*) {
        // TagArg *NextTagArg(const TagArg **tagp): returns current, advances *tagp
        const uint32_t pp = arg(0);
        uint32_t t = r32(pp), result = 0;
        for (int guard = 0; t && guard < 4096; ++guard) {
            uint32_t tag = r32(t);
            if (tag == kTagEnd) { t = 0; break; }
            if (tag == kTagJump) { t = r32(t + 4); continue; }
            if (tag == kTagNop) { t += 8; continue; }
            result = t; t += 8; break;
        }
        w32(pp, t);
        c.r[0] = result;
    };
    v[kvUSec2Ticks] = [this](Cpu*) { c.r[0] = arg(0); };
    v[kvGetSysErr] = [this](Cpu*) {
        uint32_t buf = arg(0), len = arg(1);
        char text[64]; std::snprintf(text, sizeof text, "Error 0x%08X (HLE)", arg(2));
        uint32_t n = std::min<uint32_t>(len ? len - 1 : 0, uint32_t(std::strlen(text)));
        if (len && guest_range_ok(buf, n + 1)) { rt.mem.write_block(buf, text, n); w8(buf + n, 0); }
        c.r[0] = n;
    };
    v[kvVFPrintf] = [this](Cpu*) {
        // vfprintf(stream, format, va_list): the va_list points at 32-bit
        // arguments in guest memory. Output goes to the log.
        std::string fmt = rstr(arg(1), 512), out;
        uint32_t ap = arg(2);
        if (std::getenv("TDORT_DUMP_STREAM") && guest_range_ok(arg(0), 32))
            rt.log("vfprintf stream %08X: %08X %08X %08X %08X %08X %08X %08X %08X", arg(0), r32(arg(0)), r32(arg(0) + 4),
                   r32(arg(0) + 8), r32(arg(0) + 12), r32(arg(0) + 16), r32(arg(0) + 20), r32(arg(0) + 24), r32(arg(0) + 28));
        auto next = [&]() { uint32_t v = r32(ap); ap += 4; return v; };
        for (size_t i = 0; i < fmt.size(); ++i) {
            if (fmt[i] != '%') { out += fmt[i]; continue; }
            std::string spec = "%";
            while (++i < fmt.size() && std::strchr("-+ #0123456789.", fmt[i])) spec += fmt[i];
            while (i < fmt.size() && (fmt[i] == 'l' || fmt[i] == 'h')) ++i;
            if (i >= fmt.size()) break;
            char conv = fmt[i], buf[256];
            switch (conv) {
            case 'd': case 'i': std::snprintf(buf, sizeof buf, (spec + "d").c_str(), int32_t(next())); break;
            case 'u': case 'x': case 'X': case 'o':
                std::snprintf(buf, sizeof buf, (spec + conv).c_str(), next()); break;
            case 'c': std::snprintf(buf, sizeof buf, (spec + "c").c_str(), int(next() & 0xFF)); break;
            case 's': std::snprintf(buf, sizeof buf, (spec + "s").c_str(), rstr(next(), 200).c_str()); break;
            case 'p': std::snprintf(buf, sizeof buf, "%08X", next()); break;
            case '%': std::snprintf(buf, sizeof buf, "%%"); break;
            default:  std::snprintf(buf, sizeof buf, "%%%c", conv); break;
            }
            out += buf;
        }
        // A string stream (the C library's sprintf passes one): its first
        // words are the write position, the end and the start of a buffer.
        // The text goes into that buffer instead of the log (Slam 'n Jam
        // builds knob names such as "LeftGain3" this way).
        const uint32_t st = arg(0);
        // The character-output routine is the fifth argument (on the stack;
        // the C library's wrapper passes its own caller's r3 through as the
        // fourth). When there is one, the text goes through it, as on the
        // console.
        {
            auto plausible = [&](uint32_t a) {
                if (!a || (a & 3) || a < 0x100 || !guest_range_ok(a, 8)) return false;
                return (r32(a) >> 28) == 0xE && (r32(a + 4) >> 28) == 0xE;   // unconditional ARM instructions
            };
            const uint32_t put = plausible(arg(4)) ? arg(4) : 0;
            if (put && putc_loop_ && !out.empty() && out.size() < 0x4000 && !std::getenv("TDORT_OLD_PRINTF")) {
                const uint32_t area = (4 + uint32_t(out.size()) + 3) & ~3u;
                const uint32_t sp = (c.r[13] - area) & ~3u;
                if (guest_range_ok(sp, area)) {
                    w32(sp, c.r[13] - sp);
                    rt.mem.write_block(sp + 4, out.data(), uint32_t(out.size()));
                    // Lines of text (printf) are also logged; names built with
                    // sprintf have no line break and aren't.
                    static const uint32_t kStorePut[5] = {0xE20000FF, 0xE5912000, 0xE4C20001, 0xE5812000, 0xE1A0F00E};
                    bool store = true;
                    for (int k = 0; k < 5 && store; ++k) store = r32(put + 4 * k) == kStorePut[k];
                    if (!store && out.find('\n') != std::string::npos) {
                        std::string t = out;
                        while (!t.empty() && (t.back() == '\n' || t.back() == '\r')) t.pop_back();
                        if (!t.empty()) rt.log("guest printf: %s", t.c_str());
                    }
                    c.r[13] = sp;
                    c.r[0] = st; c.r[1] = sp + 4; c.r[2] = put; c.r[3] = uint32_t(out.size());
                    c.r[15] = putc_loop_;      // returns to the caller's lr when done
                    return;
                }
            }
        }
        // The OS call also takes a character-output routine (fifth argument).
        // The C library's sprintf passes one that stores each character at a
        // pointer held in the stream word and moves it on: do that here.
        // (Cyberia builds its camera file names, "CAM000"..., this way.)
        if (const uint32_t put = arg(4); put && guest_range_ok(put, 20) && guest_range_ok(st, 4)) {
            static const uint32_t kStorePut[5] = {0xE20000FF, 0xE5912000, 0xE4C20001, 0xE5812000, 0xE1A0F00E};
            bool match = true;
            for (int k = 0; k < 5 && match; ++k) match = r32(put + 4 * k) == kStorePut[k];
            const uint32_t ptr = match ? r32(st) : 0;
            if (match && guest_range_ok(ptr, uint32_t(out.size()))) {
                rt.mem.write_block(ptr, out.data(), uint32_t(out.size()));
                w32(st, ptr + uint32_t(out.size()));
                c.r[0] = uint32_t(out.size());
                return;
            }
        }
        if (guest_range_ok(st, 12)) {
            const uint32_t pos = r32(st), end = r32(st + 4), base = r32(st + 8);
            if (base && base <= pos && pos <= end && end - base < 0x100000 && guest_range_ok(base, end - base)) {
                const uint32_t room = end - pos;
                const uint32_t n = std::min<uint32_t>(uint32_t(out.size()), room);
                rt.mem.write_block(pos, out.data(), n);
                if (n < room) w8(pos + n, 0);
                w32(st, pos + n);
                c.r[0] = uint32_t(out.size());
                return;
            }
        }
        // The older (1.x SDK) C library's string stream: the room left at +0x0C,
        // the write pointer at +0x10, a flags byte at +0x14 (bit 1: string),
        // the buffer start at +0x18. The library's sprintf adds the closing
        // zero itself through the pointer. John Madden Football builds the
        // names of the programs it launches this way.
        if (guest_range_ok(st, 0x20) && (r8(st + 0x14) & 2)) {
            const uint32_t room = r32(st + 0x0C), ptr = r32(st + 0x10), base = r32(st + 0x18);
            if (base && base <= ptr && ptr - base < 0x100000 && guest_range_ok(ptr, 1)) {
                uint32_t n = uint32_t(out.size());
                if (int32_t(room) >= 0) n = std::min(n, room);
                n = std::min<uint32_t>(n, 0x10000);
                if (guest_range_ok(ptr, n + 1)) {
                    rt.mem.write_block(ptr, out.data(), n);
                    w8(ptr + n, 0);
                    w32(st + 0x10, ptr + n);
                    if (int32_t(room) >= 0) w32(st + 0x0C, room - n);
                    c.r[0] = uint32_t(out.size());
                    return;
                }
            }
        }
        while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
        if (!out.empty()) rt.log("guest printf: %s", out.c_str());
        c.r[0] = uint32_t(out.size());
    };
    // In the 1.x SDKs WaitPort is kernel vector -24 (a SWI in later ones).
    v[-24] = [this](Cpu*) { wait_port(int32_t(arg(0)), int32_t(arg(1))); };
    v[kvStartupInit] = [](Cpu*) { /* must preserve r0/r1 (argc/argv): cstartup calls main next */ };
    v[kvExit] = [this](Cpu*) { rt.log("guest exit(%d) from \"%s\"", int32_t(c.r[0]), cur().name.c_str()); cur().exit_status = int32_t(c.r[0]); exit_current("exit()"); };

    int32_t kitem = 0;
    kbase_ = make_folio("kernel", kSubsysKernel, kKernelVectorCount, v, &kitem);
    // UniversalInsertNode(list, node, func): node goes before the first entry m
    // with func(node, m) true, else at the end. It calls back into the game, so
    // it is ARM code the runtime runs like the game's own:
    //   stmfd sp!,{r4-r7,lr}; r4=list r5=node r6=func; r7=list->head
    //   loop: if (!r7->next) goto insert (r7 is the list's anchor: append)
    //         if (func(node, r7)) goto insert; r7 = r7->next; goto loop
    //   insert: link node in before r7; ldmfd sp!,{r4-r7,pc}
    {
        static const uint32_t code[] = {
            0xE92D40F0, 0xE1A04000, 0xE1A05001, 0xE1A06002, 0xE5947014,   // 0-4
            0xE5970000, 0xE3500000, 0x0A00000D,                           // 5 loop: ldr r0,[r7]; cmp; beq tail
            0xE1A00005, 0xE1A01007, 0xE1A0E00F, 0xE1A0F006,               // 8: r0=node r1=m; call func
            0xE3500000, 0x1A000001, 0xE5977000, 0xEAFFFFF4,               // 12: bne insert; m=m->next; b loop
            0xE5970004, 0xE5857000, 0xE5850004, 0xE5805000, 0xE5875004,   // 16 insert
            0xE8BD80F0,                                                    // 21 return
            0xEAFFFFF8,                                                    // 22 tail: b insert
        };
        const uint32_t at = rt.hle_code(code, sizeof code / 4);
        w32(kbase_ + uint32_t(-17 * 4), at);
    }
    // The character-output loop for vfprintf (kernel vector -21): the C
    // library passes its own routine, putc(ch, stream), and that is what puts
    // the text where it goes (a string for sprintf, a file, the debug port).
    // The text is formatted here, left on the stack under a word holding the
    // size of that area, and this loop hands it to the routine a character
    // at a time:
    //   stmfd sp!,{r4-r8,lr}; r4=stream r5=text r6=putc r7=count r8=0
    //   loop: if (r8 >= r7) goto done; putc(text[r8], stream); r8++; goto loop
    //   done: r0=count; ldmfd sp!,{r4-r8,lr}; sp += [sp]; return
    {
        static const uint32_t code[] = {
            0xE92D41F0, 0xE1A04000, 0xE1A05001, 0xE1A06002, 0xE1A07003, 0xE3A08000,   // 0-5
            0xE1580007, 0xAA000005,                                                   // 6 loop
            0xE7D50008, 0xE1A01004, 0xE1A0E00F, 0xE1A0F006, 0xE2888001, 0xEAFFFFF7,   // 8
            0xE1A00007, 0xE8BD41F0, 0xE59DC000, 0xE08DD00C, 0xE1A0F00E,               // 14 done
        };
        putc_loop_ = rt.hle_code(code, sizeof code / 4);
    }

    // Current task and the system lists games may walk.
    HleItem& t = new_item(ItemKind::Task, kSubsysKernel, kNodeTask, task::Size, "LaunchMe", false);
    {
        HleTask main;
        main.item = t.num; main.addr = t.addr; main.name = "LaunchMe";
        // The node's priority is the one games read: libraries create their
        // threads at the caller's priority plus a step (the DataStream library
        // in Burning Soldier adds 6 and 8). Left at 0 they ran below the main
        // loop, which never waits, and the movie stalled after four blocks.
        w8(t.addr + node::Priority, main.pri);
        tasks_.push_back(main);
        cur_ = 0;
    }
    w32(cur().addr + task::StackBase, kStackTop - kStackSize);
    w32(cur().addr + task::StackSize, kStackSize);
    w32(cur().addr + task::AllocatedSigs, cur().allocated);
    w32(kbase_ + kbase::CurrentTask, cur().addr);
    for (uint32_t off : {kbase::MemFreeLists, kbase::FolioList, kbase::Devices, kbase::Tasks}) {
        uint32_t l = rt.hle_alloc(list::Size);
        list_init(l, 0);
        w32(kbase_ + off, l);
    }
    list_add_tail(r32(kbase_ + kbase::Tasks), cur().addr);
    uint32_t mem_lists = rt.hle_alloc(list::Size);
    list_init(mem_lists, 0);
    w32(cur().addr + task::FreeMemoryLists, mem_lists);
    w32(kbase_ + kbase::BootVolumeName, guest_string("cd-rom"));
    // Hardware flags of a retail console: big-endian, 32-bit mode, ARM60,
    // production ("Red") silicon, control port, no debugger. Brain Dead 13
    // refuses to run unless the Red bit is set.
    w32(kbase_ + kbase::CPUFlags, 0x1 | 0x2 | 0x4 | 0x20 | 0x1000 | 0x8000);

    // Memory bank headers: games use FindMH()/GetMemType() and the list to
    // learn where DRAM and VRAM are and what each can be used for.
    uint32_t mh_list = rt.hle_alloc(list::Size);
    list_init(mh_list, 0);
    w32(kbase_ + kbase::MemHdrList, mh_list);
    auto make_mh = [&](uint32_t base, uint32_t size, uint32_t types, const char* name) {
        uint32_t h = rt.hle_alloc(memhdr::Size);
        w8(h + node::SubsysType, kSubsysKernel);
        w8(h + node::Type, 3 /*MEMHDRNODE*/);
        w32(h + node::Size, memhdr::Size);
        w32(h + node::Name, guest_string(name));
        w32(h + memhdr::Types, types);
        w32(h + memhdr::PageSize, 2048);
        w32(h + memhdr::PageMask, 2047);
        w32(h + memhdr::VramPageSize, 2048);
        w32(h + memhdr::VramPageMask, 2047);
        w32(h + memhdr::MemBase, base);
        w32(h + memhdr::MemTop, base + size - 1);
        w8(h + memhdr::PageShift, 11);
        w8(h + memhdr::VramPageShift, 11);
        // One bit per 2 KB page, set while the page is free: the C library's
        // availMem() adds these up to report free memory.
        const uint32_t words = (size / 2048 + 31) / 32;
        w32(h + memhdr::FreePageBits, rt.hle_alloc(words * 4));
        w8(h + memhdr::FreePageBitsSize, uint8_t(words));
        list_add_tail(mh_list, h);
        return h;
    };
    memhdr_dram_ = make_mh(kDramBase, kDramSize, kMemTypesDram, "DRAM");
    memhdr_vram_ = make_mh(kVramBase, kVramSize, kMemTypesVram, "VRAM");
    update_free_page_bits();

    // Free-memory lists: the kernel's (kb_MemFreeLists) and the task's
    // (t_FreeMemoryLists) hold one MemList per memory bank, each pointing at
    // the bank's MemHdr. Road Rash's own allocator walks the kernel's list to
    // learn which bank an address belongs to; with the lists empty it followed
    // a null pointer and looped forever at start-up.
    // MemList: Node (20 bytes), Types +20, OwnBits +24, WriteBits +28,
    // MemHdr +32, l +36, Sema4 +40, OwnBitsSize +44, AlignedTrackSize +48.
    auto make_ml = [&](uint32_t list, uint32_t mh, const char* name) {
        const uint32_t ml = rt.hle_alloc(52);
        w8(ml + node::SubsysType, kSubsysKernel);
        w8(ml + node::Type, 4 /*MEMLISTNODE*/);
        w32(ml + node::Size, 52);
        w32(ml + node::Name, guest_string(name));
        w32(ml + 20, r32(mh + memhdr::Types));
        const uint32_t words = r8(mh + memhdr::FreePageBitsSize);
        w32(ml + 24, rt.hle_alloc(words * 4));      // pages owned: none yet
        w32(ml + 28, rt.hle_alloc(words * 4));      // pages writable
        w32(ml + 32, mh);
        w32(ml + 36, list);
        w8(ml + 44, uint8_t(words));
        list_add_tail(list, ml);
    };
    if (!std::getenv("TDORT_OLD_MEMLISTS")) {
        const uint32_t kl = r32(kbase_ + kbase::MemFreeLists);
        make_ml(kl, memhdr_dram_, "system dram ml");
        make_ml(kl, memhdr_vram_, "system vram ml");
        const uint32_t tl = r32(cur().addr + task::FreeMemoryLists);
        make_ml(tl, memhdr_vram_, "task vram ml");
        make_ml(tl, memhdr_dram_, "task dram ml");
    }
    (void)kitem;
}

// ============================================================================
// kernel SWIs
// ============================================================================

void Portfolio::install_kernel_swis() {
    auto K = [this](uint32_t n, const char* name, HleFn fn) { rt.register_swi(kKernelSwi + n, name, std::move(fn)); };

    K(kCreateSizedItem, "CreateSizedItem", [this](Cpu*) { c.r[0] = uint32_t(create_item(arg(0), arg(1), arg(2))); });
    // Tasks only switch at OS calls here, so Forbid/Permit (no task switching
    // until Permit) need no work.
    K(11, "Forbid", [this](Cpu*) { c.r[0] = 0; });
    K(12, "Permit", [this](Cpu*) { c.r[0] = 0; });
    K(26, "RSACheck", [this](Cpu*) { c.r[0] = 1; });                       // signature check: always valid
    K(27, "wcopy", [this](Cpu*) {                                          // wcopy(src, dst, bytes)
        const uint32_t src = arg(0), dst = arg(1);
        const uint32_t n = uint32_t(std::max<int32_t>(0, int32_t(arg(2)))) & ~3u;
        if (guest_range_ok(src, n) && guest_range_ok(dst, n)) std::memmove(&rt.mem.ram[dst], &rt.mem.ram[src], n);
        c.r[0] = 0;
    });
    K(30, "MayGetChar", [this](Cpu*) { c.r[0] = uint32_t(-1); });          // no debug terminal: no character
    K(35, "DiscOsVersion", [this](Cpu*) {                                  // DiscOsVersion(set): previous value
        const uint32_t prev = disc_os();
        if (arg(0)) disc_os_version_ = arg(0);
        c.r[0] = prev;
    });
    K(42, "Print3DOHeader", [this](Cpu*) { c.r[0] = 0; });
    // SetFunction(folio, vector, type, func) -> the previous function. Only
    // user-mode vectors (negative numbers) can be replaced.
    K(23, "SetFunction", [this](Cpu*) {
        HleItem* f = item(int32_t(arg(0)));
        const int32_t vnum = int32_t(arg(1));
        if (!f || f->kind != ItemKind::Folio || vnum >= 0 || vnum < -128) { c.r[0] = uint32_t(kErrNotSupported); return; }
        const uint32_t slot = f->addr + uint32_t(vnum * 4);
        const uint32_t old = r32(slot);
        w32(slot, arg(3));
        c.r[0] = old;
    });
    K(29, "callbacksuper", [this](Cpu*) { c.r[0] = uint32_t(kErrNotSupported); });   // supervisor code: not available
    K(34, "CompleteIO", [this](Cpu*) { c.r[0] = uint32_t(kErrNotSupported); });       // for device drivers
    K(kDeleteItem, "DeleteItem", [this](Cpu*) { c.r[0] = uint32_t(game_delete_item(int32_t(arg(0)))); });
    auto find = [this](bool open) {
        return [this, open](Cpu*) {
            const uint32_t ctype = arg(0);
            std::string name;
            each_tag(arg(1), [&](uint32_t t, uint32_t a) { if (t == kTagItemName) name = rstr(a); });
            HleItem* it = find_item(ctype, name);
            // Devices are made on first lookup, except "mac": the link to the
            // developer's Macintosh exists only on development systems, and
            // games check for it to turn on debugging aids. Gex then tried to
            // record through it after a movie and quit when that failed.
            static const auto is_dev_only = [](std::string n) {
                for (auto& ch : n) ch = char(std::tolower((unsigned char)ch));
                return n == "mac";
            };
            if (!it && ctype == node_id(kSubsysKernel, kNodeDevice) && !name.empty() && !is_dev_only(name)) {
                HleItem& d = new_item(ItemKind::Device, kSubsysKernel, kNodeDevice, device::Size, name, false);
                list_add_tail(r32(kbase_ + kbase::Devices), d.addr);
                rt.log("HLE: created device \"%s\" (item %d)", name.c_str(), d.num);
                it = &d;
            }
            // Debugging aid: TDORT_FAKE_PORTS=Name1,Name2 makes missing message
            // ports with those names exist (for running a game without one of
            // its helper programs).
            if (!it && ctype == node_id(kSubsysKernel, kNodeMsgPort) && !name.empty())
                if (const char* fake = std::getenv("TDORT_FAKE_PORTS"))
                    if (("," + std::string(fake) + ",").find("," + name + ",") != std::string::npos) {
                        HleItem& p = new_item(ItemKind::MsgPort, kSubsysKernel, kNodeMsgPort, msgport::Size, name);
                        p.owner_task = tasks_[0].item;
                        rt.log("TDORT_FAKE_PORTS: made message port \"%s\" (item %d)", name.c_str(), p.num);
                        it = &p;
                    }
            if (!it) {
                rt.log("%s(%04X, \"%s\"): not found", open ? "FindAndOpenItem" : "FindItem", ctype, name.c_str());
                c.r[0] = uint32_t(kErrNotFound);
                return;
            }
            c.r[0] = uint32_t(it->num);
        };
    };
    K(kFindItem, "FindItem", find(false));
    K(kFindAndOpenItem, "FindAndOpenItem", find(true));
    K(kOpenItem, "OpenItem", [this](Cpu*) { c.r[0] = item(int32_t(arg(0))) ? arg(0) : uint32_t(kErrBadItem); });
    K(kCloseItem, "CloseItem", [this](Cpu*) { c.r[0] = 0; });
    // LockSemaphore / UnlockSemaphore (generic LockItem/UnlockItem on other items)
    K(kLockItem, "LockSemaphore", [this](Cpu*) {
        HleItem* s = item(int32_t(arg(0)));
        if (!s || s->kind != ItemKind::Semaphore) { c.r[0] = uint32_t(kErrBadItem); return; }
        static const bool old_sem = std::getenv("TDORT_OLD_SEMAPHORE") != nullptr;
        const int32_t me = cur().item;
        if (!old_sem && s->sem_handoff == me && s->sem_owner == me) {   // handed over by UnlockSemaphore
            s->sem_handoff = 0; wait_done(); c.r[0] = 1; return;
        }
        if (s->sem_owner == 0 || s->sem_owner == me) {
            s->sem_owner = me; ++s->sem_count; wait_done(); c.r[0] = 1;
            s->sem_waiters.erase(std::remove(s->sem_waiters.begin(), s->sem_waiters.end(), me), s->sem_waiters.end());
            return;
        }
        if (arg(1) & 1) {                               // SEM_WAIT: wait for the holder
            if (std::find(s->sem_waiters.begin(), s->sem_waiters.end(), me) == s->sem_waiters.end())
                s->sem_waiters.push_back(me);
            block_retry(); return;
        }
        c.r[0] = 0;
    });
    // The kernel gives a released semaphore straight to the first task waiting
    // for it. Freeing it and letting the waiters race for it let a busier,
    // more important holder take it back every time: Quarterback Attack's
    // display thread unlocks and relocks it every frame, and the main task,
    // waiting at lower priority, never got it (the intro froze when skipped).
    // TDORT_OLD_SEMAPHORE=1: free it and let the waiters retry, as before.
    K(kUnlockItem, "UnlockSemaphore", [this](Cpu*) {
        HleItem* s = item(int32_t(arg(0)));
        static const bool old_sem = std::getenv("TDORT_OLD_SEMAPHORE") != nullptr;
        if (s && s->kind == ItemKind::Semaphore && s->sem_owner == cur().item && s->sem_count && --s->sem_count == 0) {
            s->sem_owner = 0;
            s->sem_handoff = 0;
            while (!old_sem && !s->sem_waiters.empty()) {
                const int32_t w = s->sem_waiters.front();
                s->sem_waiters.erase(s->sem_waiters.begin());
                HleTask* t = task_by_item(w);
                if (!t || t->st == HleTask::St::Dead) continue;   // the waiter is gone
                s->sem_owner = w; s->sem_count = 1; s->sem_handoff = w;
                break;
            }
            for (auto& t : tasks_) if (t.st == HleTask::St::Blocked) t.st = HleTask::St::Ready;   // waiters retry
        }
        c.r[0] = 0;
    });
    K(kSetItemPri, "SetItemPri", [this](Cpu*) {
        HleItem* it = item(int32_t(arg(0)));
        if (!it) { c.r[0] = uint32_t(kErrBadItem); return; }
        uint8_t old = r8(it->addr + node::Priority);
        w8(it->addr + node::Priority, uint8_t(arg(1)));
        // A task's priority is also the scheduler's: Need for Speed lowers its
        // main task to 80, starts a thread at 92 and spins until that thread
        // has run. Only the item's copy changed before, so the main task kept
        // 100 and the thread never ran (black screen from frame 4). A higher-
        // priority ready task now takes over at the next check (preempt()).
        if (it->kind == ItemKind::Task && !std::getenv("TDORT_OLD_SETPRI"))
            if (HleTask* t = task_by_item(it->num)) t->pri = uint8_t(arg(1));
        c.r[0] = old;
    });
    // SetItemOwner(item, task): the item now belongs to that task, so it is
    // not deleted when the old owner ends (and a port signals its new owner).
    K(kSetItemOwner, "SetItemOwner", [this](Cpu*) {
        HleItem* it = item(int32_t(arg(0)));
        if (!it || !task_by_item(int32_t(arg(1)))) { c.r[0] = uint32_t(kErrBadItem); return; }
        it->owner_task = int32_t(arg(1));
        c.r[0] = 0;
    });

    // --- signals. There is one task, so a wait can only be satisfied by
    // something the HLE itself raises (IO completion, timers) - or by time
    // passing. Waiting ends the current frame so the host stays responsive.
    K(kAllocSignal, "AllocSignal", [this](Cpu*) {
        uint32_t want = arg(0);
        if (want == 0) {
            for (uint32_t b = kSigFirstUser; b < 31; ++b)
                if (!(cur().allocated & (1u << b))) { want = 1u << b; break; }
            if (!want) { c.r[0] = uint32_t(kErrNoSignals); return; }
        } else if (cur().allocated & want) { c.r[0] = 0; return; }
        cur().allocated |= want;
        w32(cur().addr + task::AllocatedSigs, cur().allocated);
        c.r[0] = want;
    });
    K(kFreeSignal, "FreeSignal", [this](Cpu*) {
        cur().allocated &= ~(arg(0) & ~0xFFu);
        w32(cur().addr + task::AllocatedSigs, cur().allocated);
        c.r[0] = 0;
    });
    K(kSendSignal, "SendSignal", [this](Cpu*) {
        int32_t target = int32_t(arg(0));
        if (!task_by_item(target)) {
            if (stub_counts_["badsignal"]++ < 3)
                rt.log("SendSignal(%08X, %08X): not a task (caller lr=%08X r8=%08X)", target, arg(1), c.r[14], c.r[8]);
            c.r[0] = uint32_t(kErrBadItem);
            return;
        }
        raise_signal(target, arg(1));
        c.r[0] = 0;
    });
    K(kWaitSignal, "WaitSignal", [this](Cpu*) {
        const uint32_t mask = arg(0);
        uint32_t got = cur().pending & mask;
        // An empty mask has nothing to wait for: return at once (Gex's player
        // thread polls this way). Signals not in the mask stay pending.
        if (!got && mask == 0) {
            wait_done();
            c.r[0] = 0;
            return;
        }
        if (!got) {
            const uint32_t from_input = mask & listener_signals();
            const uint32_t unknown = mask & ~from_input & ~kSigAbort;
            // Another task, an input event or a timer can still raise these
            // signals: block until they do. A single task waiting on signals
            // nothing here can send would wait forever, so after a frame (or a
            // 5 second watchdog when other tasks exist) the wait is satisfied.
            // With other tasks around, waiting is normal (an idle worker thread
            // waits for its next job indefinitely): block. Only a lone task
            // waiting on signals nothing here can raise is let through.
            // Cue signals are raised by the audio engine (end of a sample, a
            // timer cue), so waiting on one always blocks.
            uint32_t from_audio = 0;
            for (auto& [cue, sig] : cue_signals_) from_audio |= sig;
            for (auto& ts : timed_signals_) if (ts.task == cur().item) from_audio |= ts.mask;
            from_audio &= mask;
            // An I/O request of this task still in progress will signal it
            // (SIGF_IODONE or its reply port's signal) when it finishes.
            bool io_pending = false;
            for (const VblIo& v : vbl_io_) if (v.task == cur().item) { io_pending = true; break; }
            if (multitasking() || from_input || from_audio || io_pending) {
                block_retry();
                return;
            }
            rt.request_vblank_wait();
            got = unknown ? unknown : mask;
        }
        wait_done();
        cur().pending &= ~got;
        w32(cur().addr + task::SigBits, cur().pending);
        c.r[0] = got;
    });
    K(kYield, "Yield", [this](Cpu*) {
        if (multitasking()) switch_to_next(false);
        else rt.request_vblank_wait();
    });

    // --- memory
    K(kAllocMemBlocks, "AllocMemBlocks", [this](Cpu*) {
        // Returns a block whose first word is its size (what FreeMemBlocks expects).
        uint32_t size = (arg(0) + 2047) & ~2047u;
        uint32_t p = alloc(size, arg(1));
        if (p) w32(p, size);
        c.r[0] = p;
    });
    // ControlMem(p, size, cmd, task). MEMC_GIVE (3) hands memory to another
    // task, so it outlives the giver, or with task 0 back to the system.
    // Samurai Shodown's fighter-select program gives the stage background and
    // the screen buffers to the fight program it starts, then exits: without
    // this they were freed with it and the fight drew from reused memory
    // (black background, dark fighters). MEMC_NOWRITE / OKWRITE change write
    // protection, which the runtime doesn't model.
    K(kControlMem, "ControlMem", [this](Cpu*) {
        const uint32_t p = arg(0), size = arg(1), cmd = arg(2);
        const int32_t to = int32_t(arg(3));
        if (cmd == 3 && size && !std::getenv("TDORT_OLD_CONTROLMEM")) {
            int32_t owner = 0;
            if (to) {
                HleTask* t = task_by_item(to);
                if (!t) { c.r[0] = uint32_t(kErrBadItem); return; }
                owner = owning_task(*t);
            }
            std::vector<uint32_t> hit;
            for (auto& [a, sz] : allocs_)
                if (a < p + size && a + sz > p) hit.push_back(a);
            // A frame buffer given away this way belongs to the new owner: the
            // screen group that allocated it no longer frees it when it goes.
            // Samurai Shodown's character select makes the screens, gives
            // their buffers to the fight program and exits; deleting its
            // screen group freed them, the fight program's next big
            // allocation landed on top of the screens it was drawing to, and
            // its sprite tables filled with pixels (dark, broken fighters).
            // TDORT_OLD_CONTROLMEM_SCREENS=1: the screen group still frees them.
            static const bool old_scr = std::getenv("TDORT_OLD_CONTROLMEM_SCREENS") != nullptr;
            for (uint32_t a : hit) {
                if (owner) {
                    if (!old_scr && alloc_owner_[a] != owner)
                        for (HleItem& it : items_)
                            if (it.alive && it.kind == ItemKind::Bitmap && it.owns_buffer && it.buffer == a) it.owns_buffer = false;
                    alloc_owner_[a] = owner;
                }
                else if (a >= p && a + allocs_[a] <= p + size) free_mem(a);   // whole blocks back to the system
            }
            rt.log("ControlMem: %s %08X+%X (%zu block(s))", owner ? "gave" : "returned", p, size, hit.size());
        }
        c.r[0] = 0;
    });
    K(kSystemScavengeMem, "SystemScavengeMem", [this](Cpu*) { c.r[0] = 0; });

    // --- messages
    // Message data: a pass-by-value message gets a copy in its own area.
    auto set_msg_data = [this](HleItem& msg, uint32_t data, uint32_t size) {
        if (msg.msg_buffer) {
            const uint32_t n = std::min(size, msg.msg_buffer_size);
            if (n && data && data != msg.msg_buffer && guest_range_ok(data, n)) {
                std::vector<uint8_t> tmp(rt.mem.ram.begin() + data, rt.mem.ram.begin() + data + n);
                rt.mem.write_block(msg.msg_buffer, tmp.data(), n);
            }
            w32(msg.addr + message::DataPtr, msg.msg_buffer);
            w32(msg.addr + message::DataSize, n);
        } else {
            w32(msg.addr + message::DataPtr, data);
            w32(msg.addr + message::DataSize, size);
        }
    };
    K(kSendMsg, "SendMsg", [this, set_msg_data](Cpu*) {
        HleItem* port = item(int32_t(arg(0)));
        HleItem* msg = item(int32_t(arg(1)));
        if (!port || !msg || port->kind != ItemKind::MsgPort) { c.r[0] = uint32_t(kErrBadItem); return; }
        static const bool log_data = std::getenv("TDORT_LOG_MSGDATA") != nullptr;   // debugging aid
        if (log_data && arg(2) && arg(3) >= 8 && arg(3) <= 64 && arg(2) + arg(3) <= kRamEnd)
            rt.log("SendMsg port %d msg %d from %s (frame %u): %08X %08X %08X %08X %08X %08X", port->num, msg->num,
                   cur().name.c_str(), frame_no_, r32(arg(2)), r32(arg(2) + 4), r32(arg(2) + 8), r32(arg(2) + 12),
                   arg(3) >= 20 ? r32(arg(2) + 16) : 0, arg(3) >= 24 ? r32(arg(2) + 20) : 0);
        set_msg_data(*msg, arg(2), arg(3));
        if (port->num == broker_port_) {
            broker_receive(*msg, r32(msg->addr + message::DataPtr), r32(msg->addr + message::DataSize));
            c.r[0] = 0;
            return;
        }
        send_message(*port, *msg);
        c.r[0] = 0;
    });
    K(kGetMsg, "GetMsg", [this](Cpu*) {
        HleItem* port = item(int32_t(arg(0)));
        if (!port || port->queue.empty()) { c.r[0] = 0; return; }
        int32_t m = port->queue.front(); port->queue.pop_front();
        if (HleItem* msg = item(m)) unqueue(*msg);
        c.r[0] = uint32_t(m);
    });
    K(kGetThisMsg, "GetThisMsg", [this](Cpu*) {
        HleItem* msg = item(int32_t(arg(0)));
        if (!msg) { c.r[0] = uint32_t(kErrBadItem); return; }
        if (HleItem* port = item(msg->in_port)) {
            auto& q = port->queue;
            q.erase(std::remove(q.begin(), q.end(), msg->num), q.end());
        }
        unqueue(*msg);
        c.r[0] = uint32_t(msg->num);
    });
    K(kReplyMsg, "ReplyMsg", [this, set_msg_data](Cpu*) {
        HleItem* msg = item(int32_t(arg(0)));
        if (!msg) { c.r[0] = uint32_t(kErrBadItem); return; }
        static const bool log_data = std::getenv("TDORT_LOG_MSGDATA") != nullptr;   // debugging aid
        if (log_data) rt.log("ReplyMsg msg %d result %08X from %s (frame %u) to port %d", msg->num, arg(1), cur().name.c_str(), frame_no_, msg->reply_port);
        w32(msg->addr + message::Result, arg(1));
        set_msg_data(*msg, arg(2), arg(3));
        if (HleItem* port = item(msg->reply_port)) send_message(*port, *msg);
        c.r[0] = 0;
    });
    K(kWaitPort, "WaitPort", [this](Cpu*) { wait_port(int32_t(arg(0)), int32_t(arg(1))); });

    // --- IO
    // A request can put the caller to sleep (timer delays) and switch to
    // another task inside do_io, so the result must go to the caller's saved
    // registers, not to whichever task is now on the CPU.
    auto to_caller = [this](size_t me, uint32_t v) {
        if (cur_ == me && !idle_) c.r[0] = v;
        else if (me < tasks_.size()) tasks_[me].r[0] = v;
    };
    K(kSendIO, "SendIO", [this, to_caller](Cpu*) {
        const size_t me = cur_;
        bool quick = false;
        int32_t err = do_io(int32_t(arg(0)), arg(1), &quick);
        to_caller(me, err < 0 ? uint32_t(err) : (quick ? 1u : 0u));
    });
    K(kDoIO, "DoIO", [this, to_caller](Cpu*) {
        const size_t me = cur_;
        to_caller(me, uint32_t(do_io(int32_t(arg(0)), arg(1), nullptr)));
    });
    // WaitIO(ior): as the kernel does, waits until the request is done, then
    // takes its completion message off the reply port, or uses up the
    // SIGF_IODONE signal. (It used to return at once, even mid-request.)
    K(kWaitIO, "WaitIO", [this](Cpu*) {
        HleItem* ior = item(int32_t(arg(0)));
        if (!ior || ior->kind != ItemKind::IOReq) { c.r[0] = uint32_t(kErrBadItem); return; }
        if (old_io()) { c.r[0] = r32(ior->addr + ioreq::Error); return; }
        const uint32_t flags = r32(ior->addr + ioreq::Flags);
        if (!(flags & kIoQuick) && !(flags & kIoDone)) { block_retry(); return; }
        if (!(flags & kIoQuick)) {
            if (HleItem* port = item(ior->reply_port)) {
                auto& q = port->queue;
                auto it = std::find(q.begin(), q.end(), ior->msg_item);
                if (it != q.end()) { q.erase(it); if (HleItem* m = item(ior->msg_item)) m->in_port = 0; }
            } else {
                cur().pending &= ~kSigIODone;
                w32(cur().addr + task::SigBits, cur().pending);
            }
        }
        wait_done();
        c.r[0] = r32(ior->addr + ioreq::Error);
    });
    // AbortIO(ior): a request still pending (a timer, a SPORT transfer) ends
    // now with ER_Aborted; a finished one is left alone.
    K(kAbortIO, "AbortIO", [this](Cpu*) {
        HleItem* ior = item(int32_t(arg(0)));
        if (!ior || ior->kind != ItemKind::IOReq) { c.r[0] = uint32_t(kErrBadItem); return; }
        if (std::getenv("TDORT_DEBUG_IO")) { std::string l; for (auto& v : vbl_io_) l += " " + std::to_string(v.ior) + "@" + std::to_string(v.due_frame); rt.log("AbortIO %d at frame %u: pending%s", ior->num, frame_no_, l.c_str()); }
        for (size_t i = 0; i < vbl_io_.size(); ++i) {
            if (vbl_io_[i].ior != ior->num) continue;
            const int32_t task = vbl_io_[i].task;
            vbl_io_.erase(vbl_io_.begin() + long(i));
            const bool q = io_quick_;
            io_quick_ = false;
            complete_io(*ior, int32_t(0x8101010A) /* ER_Aborted */, 0, task);
            io_quick_ = q;
            break;
        }
        c.r[0] = 0;
    });

    // --- misc
    // SampleSystemTime(): the seconds of the system clock (the timer device's
    // microsecond unit) in r0, the microseconds in r1. Returning microseconds
    // in r0 sent Primal Rage's movie player to sleep for days (it adds a
    // frame time to {r0, r1} and waits for that clock time). TDORT_OLD_SAMPLETIME=1: old.
    K(kSampleSystemTime, "SampleSystemTime", [this](Cpu*) {
        static const bool old_st = std::getenv("TDORT_OLD_SAMPLETIME") != nullptr;
        const uint64_t us = usec();
        if (old_st) { c.r[0] = uint32_t(us); return; }
        c.r[0] = uint32_t(us / 1000000u);
        c.r[1] = uint32_t(us % 1000000u);
    });
    K(kReadHwRandom, "ReadHardwareRandomNumber", [this](Cpu*) {
        uint32_t& x = hw_random_;
        x ^= x << 13; x ^= x >> 17; x ^= x << 5;
        c.r[0] = x;
    });
    K(kSetExitStatus, "SetExitStatus", [this](Cpu*) { cur().exit_status = int32_t(arg(0)); c.r[0] = 0; });
    // SWI 14 is not in the public headers; the C library's putc() issues it
    // for stdout/stderr with the character in r1 (seen in Gex). Debug console.
    K(14, "DebugPutChar", [this](Cpu*) {
        // Some libraries pass a format in r0 and its arguments in r1-r3
        // (FIFA's debug messages: "%s", text): that is a line of text.
        if (c.r[0] >= 0x100 && guest_range_ok(c.r[0], 2) && c.r[1] >= 0x100) {
            const std::string fmt = rstr(c.r[0], 256);
            if (!fmt.empty() && fmt.find('%') != std::string::npos) {
                std::string out;
                int a = 1;
                for (size_t i = 0; i < fmt.size(); ++i) {
                    if (fmt[i] != '%' || i + 1 >= fmt.size()) { out += fmt[i]; continue; }
                    size_t j = i + 1;
                    while (j < fmt.size() && std::strchr("-+ #0123456789.l", fmt[j])) ++j;
                    const char conv = j < fmt.size() ? fmt[j] : 0;
                    const uint32_t v = a <= 3 ? c.r[a] : 0;
                    char b[64];
                    if (conv == 's') { ++a; out += guest_range_ok(v, 1) ? rstr(v, 240) : "(?)"; }
                    else if (conv == 'd' || conv == 'i') { ++a; std::snprintf(b, sizeof b, "%d", int32_t(v)); out += b; }
                    else if (conv == 'x' || conv == 'X' || conv == 'p') { ++a; std::snprintf(b, sizeof b, "%x", v); out += b; }
                    else if (conv == 'c') { ++a; out += char(v); }
                    else if (conv == '%') out += '%';
                    i = j;
                }
                while (!out.empty() && (out.back() == '\n' || out.back() == '\r')) out.pop_back();
                rt.log("guest stdout: %s", out.c_str());
                return;
            }
        }
        const char ch = char(c.r[1] & 0xFF);
        if (ch == '\n' || debug_line_.size() > 240) { rt.log("guest stdout: %s", debug_line_.c_str()); debug_line_.clear(); }
        if (ch != '\n' && ch != '\r') debug_line_ += ch;
    });
}

// A message taken off its port: msg_MsgPort says where a message currently
// is, and games (WaitIO in the 1.x libraries) compare it with the reply port.
void Portfolio::unqueue(HleItem& msg) {
    msg.in_port = 0;
    w32(msg.addr + message::MsgPort, 0);
}

// WaitPort(port, msg): wait for msg (or any message if msg <= 0) on port.
// With no port, wait for msg wherever it is going (its reply port).
// An I/O request still in progress (a disc read waiting for the drive, a
// timer) that will reply to this port, or with this message.
bool Portfolio::io_will_reply(int32_t port_num, int32_t msg_num) {
    for (const VblIo& v : vbl_io_)
        if (HleItem* ior = item(v.ior)) {
            if (port_num > 0 && ior->reply_port == port_num) return true;
            if (msg_num > 0 && ior->msg_item == msg_num) return true;
        }
    return false;
}

void Portfolio::wait_port(int32_t port_num, int32_t want) {
    if (port_num <= 0 && want > 0) {
        // No port: wait until this message has been replied, then take it
        // off whatever port it went to.
        HleItem* m = item(want);
        if (!m) { c.r[0] = uint32_t(kErrBadItem); return; }
        if (io_will_reply(0, want)) { block_retry(); return; }
        if (HleItem* p = item(m->in_port)) {
            auto& q = p->queue;
            q.erase(std::remove(q.begin(), q.end(), want), q.end());
        }
        unqueue(*m);
        wait_done();
        c.r[0] = uint32_t(want);
        return;
    }
    HleItem* port = item(port_num);
    if (!port) { c.r[0] = uint32_t(kErrBadItem); return; }
    auto& q = port->queue;
    auto it = want > 0 ? std::find(q.begin(), q.end(), want) : q.begin();
    if (it == q.end()) {
        // A disc read still on its way replies here: wait for it, even with
        // one task (The Coven and Plumbers Don't Wear Ties read their movies
        // this way and quit with a DataStream error when WaitPort gave up).
        if (blocking_port(port->num) || multitasking() || io_will_reply(port->num, want)) { block_retry(); return; }
        rt.log("WaitPort(%d \"%s\", %d): nothing queued and no other task can send; returning error",
               port->num, port->name.c_str(), want);
        rt.request_vblank_wait();
        c.r[0] = uint32_t(kErrNotFound);
        return;
    }
    int32_t m = *it; q.erase(it);
    if (HleItem* msg = item(m)) unqueue(*msg);
    wait_done();
    c.r[0] = uint32_t(m);
}

void Portfolio::send_message(HleItem& port, HleItem& msg) {
    if (port.num == broker_port_ && broker_buf_.count(msg.num)) {   // game replied to an event
        last_input_frame_ = frame_no_;
        msg.in_port = 0;
        broker_free_.push_back(msg.num);
        return;
    }
    port.queue.push_back(msg.num);
    msg.in_port = port.num;
    w32(msg.addr + message::MsgPort, uint32_t(port.num));
    // The port's signal belongs to the task that created the port.
    raise_signal(port.owner_task ? port.owner_task : cur().item, port.signal);
}

int32_t Portfolio::create_item(uint32_t ctype, uint32_t tags, uint32_t size) {
    const uint8_t subsys = uint8_t(ctype >> 8), type = uint8_t(ctype);
    std::string name;
    uint32_t pri = 0;
    each_tag(tags, [&](uint32_t t, uint32_t a) {
        if (t == kTagItemName) name = rstr(a);
        else if (t == kTagItemPri) pri = a;
    });

    if (subsys == kSubsysKernel) {
        switch (type) {
        case kNodeIOReq: {
            uint32_t dev = 0, reply = 0;
            tag_value(tags, kTagIOReqDevice, dev);
            tag_value(tags, kTagIOReqReplyPort, reply);
            HleItem* d = item(int32_t(dev));
            if (!d) { rt.log("CreateIOReq: bad device item %d", int32_t(dev)); return kErrBadItem; }
            HleItem& io = new_item(ItemKind::IOReq, kSubsysKernel, kNodeIOReq, std::max(size, ioreq::Size), name);
            const int32_t io_num = io.num;
            const uint32_t io_addr = io.addr;
            item(io_num)->device = d->num;
            item(io_num)->reply_port = int32_t(reply);
            w32(io_addr + ioreq::Dev, d->addr);
            w32(io_addr + ioreq::Flags, kIoDone);     // a new request has nothing pending
            // An IOReq with a reply port owns a message item, used to reply
            // when it completes. Without one, io_MsgItem stays 0 and the caller
            // gets SIGF_IODONE instead: the SDK's WaitIO looks at the message's
            // reply port only when there is a message, otherwise it waits for
            // the signal (Real Pinball's file loader depends on this).
            if (reply) {
                HleItem& m = new_item(ItemKind::Message, kSubsysKernel, kNodeMessage, message::Size, "");
                m.reply_port = int32_t(reply);
                w32(m.addr + message::ReplyPort, reply);
                item(io_num)->msg_item = m.num;
                w32(io_addr + ioreq::MsgItem, uint32_t(m.num));
            }
            return io_num;
        }
        case kNodeMsgPort: {
            uint32_t sig = 0;
            if (!tag_value(tags, kTagPortSignal, sig)) {
                for (uint32_t b = kSigFirstUser; b < 31; ++b)
                    if (!(cur().allocated & (1u << b))) { sig = 1u << b; break; }
                cur().allocated |= sig;
            }
            HleItem& p = new_item(ItemKind::MsgPort, kSubsysKernel, kNodeMsgPort, std::max(size, msgport::Size), name);
            p.signal = sig;
            w32(p.addr + msgport::Signal, sig);
            list_init(p.addr + msgport::Msgs, 0);
            return p.num;
        }
        case kNodeMessage: {
            uint32_t reply = 0, data_size = 0;
            tag_value(tags, kTagMsgReplyPort, reply);
            // A pass-by-value message (CREATEMSG_TAG_DATA_SIZE) carries its own
            // data area after the message: SendMsg and ReplyMsg copy the data
            // into it, and replies such as the event broker's pod list land there.
            tag_value(tags, kTagMsgDataSize, data_size);
            if (data_size > 0x100000) data_size = 0;
            HleItem& m = new_item(ItemKind::Message, kSubsysKernel, kNodeMessage, std::max(size, message::Size) + data_size, name);
            m.reply_port = int32_t(reply);
            w32(m.addr + message::ReplyPort, reply);
            if (data_size && std::getenv("TDORT_DEBUG_MSG"))
                each_tag(tags, [&](uint32_t t, uint32_t a) { rt.log("  message %d tag %u = %08X", m.num, t, a); });
            if (data_size) {
                m.msg_buffer = m.addr + std::max(size, message::Size);
                m.msg_buffer_size = data_size;
                w32(m.addr + message::DataPtr, m.msg_buffer);
                w32(m.addr + message::DataPtrSize, data_size);
                w8(m.addr + node::Flags, uint8_t(r8(m.addr + node::Flags) | 0x08));   // MESSAGE_PASS_BY_VALUE
            }
            // A small message carries two values in DataPtr/DataSize; receivers
            // check MESSAGE_SMALL (Digital Pictures' movie engine rejects the
            // message without it: "Video: Unknown message!").
            bool is_small = false;
            each_tag(tags, [&](uint32_t t, uint32_t) { if (t == kTagMsgIsSmall) is_small = true; });
            if (is_small && !data_size)
                w8(m.addr + node::Flags, uint8_t(r8(m.addr + node::Flags) | 0x04));   // MESSAGE_SMALL
            return m.num;
        }
        case kNodeTask: {
            // A thread can be a little more important than its creator, as
            // libraries make their workers (the DataStream library: creator + 6
            // to + 11; Escape from Monster Manor and Burning Soldier stream
            // movies that way). A request above the user range (10-199) is
            // capped to the creator's: games ask for 255 and then busy-wait on
            // the parent (Slam 'n Jam's menu thread spins until the main task
            // advances a state flag), which only works if the parent can run.
            // A valid priority is the game's design and is kept. Shock Wave 2's audio-setup thread asks
            // for 119 under a 99 main task and must take each sound chunk the
            // main task hands it before the next one overwrites the slot.
            // TDORT_OLD_PRI_CAP=1: cap any jump of more than 15.
            static const bool old_cap = std::getenv("TDORT_OLD_PRI_CAP") != nullptr;
            uint32_t p = pri ? pri : 100;
            if (!idle_ && p > uint32_t(cur().pri) + 15 && (old_cap || p > 199)) {
                rt.log("CreateThread(\"%s\"): priority %u capped to the creator's %u", name.c_str(), p, unsigned(cur().pri));
                p = cur().pri;
            }
            return create_task(name, tags, uint8_t(p));
        }
        case kNodeSemaphore: {
            HleItem& sm = new_item(ItemKind::Semaphore, kSubsysKernel, kNodeSemaphore, std::max<uint32_t>(size, 64), name);
            return sm.num;
        }
        default: break;
        }
    }
    if (subsys == kSubsysAudio) return create_audio_item(type, tags, name);
    // CreateBitmap(tags): a bitmap over a buffer the program supplies
    // (CBM_TAG_WIDTH 11, HEIGHT 12, BUFFER 13, CLIPWIDTH..CLIPY 14-17,
    // CECONTROL 19). FIFA's movie player decodes straight into one.
    if (subsys == kSubsysGraphics && type == kNodeBitmap && !std::getenv("TDORT_OLD_BITMAP")) {
        uint32_t w = 320, h = 240, buf = 0, cw = 0, ch = 0, cx = 0, cy = 0, ce = 0;
        bool has_cw = false, has_ch = false, has_ce = false;
        each_tag(tags, [&](uint32_t t, uint32_t a) {
            switch (t) {
            case 11: w = a; break;
            case 12: h = a; break;
            case 13: buf = a; break;
            case 14: cw = a; has_cw = true; break;
            case 15: ch = a; has_ch = true; break;
            case 16: cx = a; break;
            case 17: cy = a; break;
            case 19: ce = a; has_ce = true; break;
            default: break;
            }
        });
        if (w == 0 || h == 0 || w > 2048 || h > 2048) return kErrNoMem;
        if (!buf) buf = alloc(w * h * 2, kMemVram);
        HleItem& bm = new_item(ItemKind::Bitmap, kSubsysGraphics, kNodeBitmap, std::max(size, bitmap::Size), name);
        bm.buffer = buf;
        w32(bm.addr + bitmap::Buffer, buf);
        w32(bm.addr + bitmap::Width, w);
        w32(bm.addr + bitmap::Height, h);
        w32(bm.addr + bitmap::ClipWidth, has_cw ? cw : w);
        w32(bm.addr + bitmap::ClipHeight, has_ch ? ch : h);
        w32(bm.addr + bitmap::ClipX, cx);
        w32(bm.addr + bitmap::ClipY, cy);
        if (has_ce) w32(bm.addr + bitmap::CEControl, ce);
        rt.log("CreateBitmap(%ux%u, buffer %08X) -> %d", w, h, buf, bm.num);
        return bm.num;
    }
    rt.log("CreateItem(%04X \"%s\"): generic item", ctype, name.c_str());
    HleItem& g = new_item(ItemKind::Generic, subsys, type, std::max<uint32_t>(size, node::ItemNodeSize), name);
    w8(g.addr + node::Priority, uint8_t(pri));
    return g.num;
}

// ============================================================================
// IO dispatch (everything completes synchronously)
// ============================================================================

// Every IO here finishes immediately. As on the real kernel, a request made
// with IO_QUICK that completes synchronously only gets IO_DONE|IO_QUICK set:
// no signal, no message (the caller's WaitIO sees IO_QUICK and returns). Other
// requests are completed the asynchronous way: SIGF_IODONE, or a reply to the
// IOReq's reply port.
// The OS version the disc was made with (DiscOsVersion): set by the game,
// otherwise the version and revision of the disc's kernel ("sherry", rom tag
// type 7) in its rom_tags file. Old discs leave the version 0.
uint32_t Portfolio::disc_os() {
    if (disc_os_version_) return disc_os_version_;
    if (!disc_os_read_) {
        disc_os_read_ = true;
        if (FILE* f = std::fopen((fs::u8path(disc_root()) / "rom_tags").u8string().c_str(), "rb")) {
            uint8_t t[32];
            while (std::fread(t, 1, 32, f) == 32)
                if (t[0] == 0x0F && t[1] == 0x07) { disc_os_rom_ = (uint32_t(t[2]) << 8) | t[3]; break; }
            std::fclose(f);
        }
        rt.log("disc OS version %u.%u (rom_tags)", disc_os_rom_ >> 8, disc_os_rom_ & 255);
    }
    return disc_os_rom_;
}

void Portfolio::complete_io(HleItem& ior, int32_t err, uint32_t actual, int32_t task_item) {
    HleTask* owner = task_item ? task_by_item(task_item) : &cur();
    if (!owner) owner = &cur();
    w32(ior.addr + ioreq::Actual, actual);
    w32(ior.addr + ioreq::Error, uint32_t(err));
    if (io_quick_) {
        w32(ior.addr + ioreq::Flags, r32(ior.addr + ioreq::Flags) | kIoDone | kIoQuick);
        return;
    }
    w32(ior.addr + ioreq::Flags, r32(ior.addr + ioreq::Flags) | kIoDone);
    if (!ior.reply_port) raise_signal(owner->item, kSigIODone);
    if (HleItem* port = item(ior.reply_port)) {
        HleItem* m = item(ior.msg_item);
        if (!m) return;
        // The completion message carries the IOReq's item number (games pass it to LookupItem).
        // As the kernel does: for a disc made with an OS newer than 1.4 the
        // message is a small one (MESSAGE_SMALL) with io_Error as its result
        // and ioi_User as its size; for older discs (Digital Pictures' Night
        // Trap engine) it is a plain message with result and size 0.
        // TDORT_OLD_IO_MSG=1: plain message, result io_Error, size untouched.
        static const bool old_msg = std::getenv("TDORT_OLD_IO_MSG") != nullptr;
        w32(m->addr + message::DataPtr, uint32_t(ior.num));
        if (old_msg) {
            w32(m->addr + message::Result, uint32_t(err));
        } else if (disc_os() > 0x170A) {    // DiscOs_1_4 = version 23.10
            w8(m->addr + node::Flags, uint8_t(r8(m->addr + node::Flags) | 0x04));
            w32(m->addr + message::Result, uint32_t(err));
            w32(m->addr + message::DataSize, r32(ior.addr + ioreq::Info + ioinfo::User));
        } else {
            w32(m->addr + message::Result, 0);
            w32(m->addr + message::DataSize, 0);
        }
        send_message(*port, *m);
    }
    w32(owner->addr + task::SigBits, owner->pending);
}

int32_t Portfolio::do_io(int32_t ior_num, uint32_t info, bool* quick) {
    HleItem* ior = item(ior_num);
    if (!ior || ior->kind != ItemKind::IOReq) return kErrBadItem;
    HleItem* dev = item(ior->device);
    if (!dev) return kErrBadItem;
    // Copy the caller's IOInfo into the request, as the kernel does.
    for (uint32_t k = 0; k < ioinfo::Size; k += 4) w32(ior->addr + ioreq::Info + k, r32(info + k));
    w32(ior->addr + ioreq::Flags, 0);
    // A new request starts with no error and nothing transferred, as the
    // kernel does. A request still pending reports 0 from SendIO; a stale
    // ER_Aborted left by AbortIO made Creature Shock skip a reply and hang.
    w32(ior->addr + ioreq::Error, 0);
    w32(ior->addr + ioreq::Actual, 0);
    // DoIO (quick == nullptr) always asks for synchronous completion.
    io_quick_ = !quick || (r8(info + ioinfo::Flags) & kIoQuick) != 0;
    if (quick) *quick = io_quick_;

    if (dev->kind == ItemKind::File) file_io(*ior, *dev, ior->addr + ioreq::Info);
    else if (lower(dev->name) == "timer") timer_io(*ior, ior->addr + ioreq::Info);
    else if (lower(dev->name) == "sport") sport_io(*ior, ior->addr + ioreq::Info);
    else if (lower(dev->name) == "cd-rom" && r8(info + ioinfo::Command) == kCmdStatus) {
        // The drive's status: games poll it to notice a removed disc (Real
        // Pinball's music player stops when it doesn't see "disc in, ready").
        // Flag byte: door closed, disc in, spun up, double speed, ready.
        uint8_t st[devstatus::Size] = {};
        store_be32(st + devstatus::MaxStatusSize, devstatus::Size);
        store_be32(st + devstatus::BlockSize, 2048);
        store_be32(st + devstatus::BlockCount, 330000);
        store_be32(st + devstatus::FlagWord, 0xE3u);         // status byte is the low byte
        store_be32(st + devstatus::UsageFlags, 0x3);
        const uint32_t rbuf = r32(info + ioinfo::RecvBuf), rlen = r32(info + ioinfo::RecvLen);
        const uint32_t n = std::min<uint32_t>(rlen, devstatus::Size);
        if (guest_range_ok(rbuf, n)) rt.mem.write_block(rbuf, st, n);
        complete_io(*ior, 0, n);
    }
    else {
        stub("IO on device \"" + dev->name + "\" cmd " + std::to_string(r8(info + ioinfo::Command)));
        complete_io(*ior, 0, 0);
    }
    return int32_t(r32(ior->addr + ioreq::Error));
}

// SPORT: the VRAM block mover. CLONE repeats one source page across the
// destination (how games clear the screen); COPY moves pages. ioi_Offset is
// a write mask applied to every 32-bit word.
void Portfolio::sport_io(HleItem& ior, uint32_t info) {
    const uint8_t cmd = r8(info + ioinfo::Command);
    const uint32_t mask = r32(info + ioinfo::Offset);
    const uint32_t src = r32(info + ioinfo::SendBuf), slen = r32(info + ioinfo::SendLen);
    const uint32_t dst = r32(info + ioinfo::RecvBuf), dlen = r32(info + ioinfo::RecvLen);
    if (cmd == 6 && guest_range_ok(dst, dlen)) {        // FLASHWRITE: fill with a constant
        uint32_t m = r32(info + ioinfo::CmdOptions);
        if (m == 0) m = 0xFFFFFFFFu;
        for (uint32_t k = 0; k + 4 <= dlen; k += 4) {
            uint8_t* d = &rt.mem.ram[dst + k];
            store_be32(d, (mask & m) | (load_be32(d) & ~m));
        }
        rt.mem.invalidate(dst, dlen);
        rt.gfx->note_write(dst, dlen);
        complete_io(ior, 0, dlen);
        return;
    }
    if ((cmd != 4 && cmd != 5) || !slen || !guest_range_ok(src, slen) || !guest_range_ok(dst, dlen)) {
        stub("SPORT cmd " + std::to_string(cmd));
        complete_io(ior, 0, 0);
        return;
    }
    const uint32_t n = cmd == 5 ? std::min(slen, dlen) : dlen;
    if (rt.cfg.trace) rt.log("SPORT cmd %u: %08X/%u -> %08X-%08X", cmd, src, slen, dst, dst + n);
    for (uint32_t k = 0; k + 4 <= n; k += 4) {
        const uint32_t s = load_be32(&rt.mem.ram[src + (cmd == 4 ? k % slen : k)]);
        uint8_t* d = &rt.mem.ram[dst + k];
        store_be32(d, mask == 0xFFFFFFFFu ? s : ((s & mask) | (load_be32(d) & ~mask)));
    }
    rt.mem.invalidate(dst, n);
    rt.gfx->note_write(dst, n);
    // The SPORT bus moves VRAM during vertical blank, so an asynchronous
    // request finishes at the next one. Games rely on the wait: Slam 'n Jam's
    // draw loop blocks on it each frame, which is when its low-priority input
    // thread gets to run. Completing it at once starved that thread.
    if (!io_quick_) {
        w32(ior.addr + ioreq::Flags, r32(ior.addr + ioreq::Flags) & ~kIoDone);
        vbl_io_.push_back({ior.num, cur().item, 0, n, frame_no_ + 1});
        return;
    }
    complete_io(ior, 0, n);
}

// An asynchronous (SendIO) request that completes `frames` frames from now,
// as a real timer request does: the caller keeps running, and the IOReq is
// marked done (with its signal or reply) when the time comes.
// TDORT_OLD_IO=1: timer SendIO sleeps the caller and WaitIO returns at once,
// as before November 2026 (a fallback if a game does worse with the change).
bool Portfolio::old_io() { static const bool v = std::getenv("TDORT_OLD_IO") != nullptr; return v; }

void Portfolio::defer_io(HleItem& ior, uint32_t frames) {
    w32(ior.addr + ioreq::Flags, r32(ior.addr + ioreq::Flags) & ~kIoDone);
    vbl_io_.push_back({ior.num, cur().item, 0, 0, frame_no_ + frames});
    if (rt.cfg.trace) rt.log("timer IO %d: completes in %u frames (at frame %u)", ior.num, frames, frame_no_ + frames);
}

void Portfolio::timer_io(HleItem& ior, uint32_t info) {
    const uint8_t cmd = r8(info + ioinfo::Command), unit = r8(info + ioinfo::Unit);
    if (stub_counts_["timer " + std::to_string(cmd) + "/" + std::to_string(unit)]++ < 3)
        rt.log("timer request: cmd=%u unit=%u offset=%u send=%08X/%u recv=%08X/%u", cmd, unit, r32(info + ioinfo::Offset),
               r32(info + ioinfo::SendBuf), r32(info + ioinfo::SendLen), r32(info + ioinfo::RecvBuf), r32(info + ioinfo::RecvLen));
    if ((cmd == kTimerCmdDelay || cmd == kTimerCmdDelayUntil) && unit == kTimerUnitVblank) {
        // WaitVBL and friends: this task sleeps until the next frame(s).
        uint32_t fields = r32(info + ioinfo::Offset);
        uint32_t frames = cmd == kTimerCmdDelay ? std::max<uint32_t>(1, (fields + 1) / 2) : 1;
        if (!io_quick_ && multitasking() && !old_io()) { defer_io(ior, frames); return; }   // SendIO: the caller carries on
        complete_io(ior, 0, 0);
        if (multitasking()) sleep_until(frame_no_ + frames);
        else rt.request_vblank_wait();
        return;
    }
    if (cmd == kCmdRead) {                        // current time as {seconds, microseconds}
        uint32_t buf = r32(info + ioinfo::RecvBuf), len = r32(info + ioinfo::RecvLen);
        uint64_t us = usec();
        if (unit == kTimerUnitVblank) {
            // VBlankTimeVal: a 64-bit count, high word first. Jurassic Park's
            // terminal busy-waits on the low word (it was always 0 here).
            const uint64_t vbl = us / 16667;
            if (len >= 8) { w32(buf, uint32_t(vbl >> 32)); w32(buf + 4, uint32_t(vbl)); }
            else if (len >= 4) w32(buf, uint32_t(vbl));
        }
        else { if (len >= 4) w32(buf, uint32_t(us / 1000000)); if (len >= 8) w32(buf + 4, uint32_t(us % 1000000)); }
        complete_io(ior, 0, std::min<uint32_t>(len, 8));
        return;
    }
    if (cmd == kTimerCmdDelay || cmd == kTimerCmdDelayUntil) {
        // Microsecond unit: the task sleeps that long ({seconds, microseconds} in
        // the send buffer: a duration for DELAY, a clock time for DELAYUNTIL).
        // Tasks are scheduled by frame, so the delay is rounded to whole
        // frames; under half a frame it only yields. Completing at once made
        // Doom's 60 Hz timer thread race: its game loop waits on that tick.
        const uint32_t buf = r32(info + ioinfo::SendBuf), len = r32(info + ioinfo::SendLen);
        uint64_t want = 0;
        if (buf && len >= 8) want = uint64_t(r32(buf)) * 1000000u + r32(buf + 4);
        else if (buf && len >= 4) want = uint64_t(r32(buf)) * 1000000u;
        uint64_t us = want;
        if (cmd == kTimerCmdDelayUntil) { const uint64_t now = usec(); us = want > now ? want - now : 0; }
        const uint32_t frames = uint32_t(std::min<uint64_t>((us + 8333) / 16667, 60u * 3600u));
        if (rt.cfg.trace) rt.log("timer IO %d: %s %llu us (now %llu) -> %u frames", ior.num, cmd == kTimerCmdDelayUntil ? "until" : "delay",
                                 (unsigned long long)want, (unsigned long long)usec(), frames);
        if (!io_quick_ && multitasking() && !old_io()) { defer_io(ior, std::max<uint32_t>(frames, 1)); return; }   // SendIO: the caller carries on
        complete_io(ior, 0, 0);
        if (frames == 0) { if (multitasking()) switch_to_next(false); return; }
        if (multitasking()) sleep_until(frame_no_ + frames);
        else rt.request_vblank_wait();
        return;
    }
    stub("timer cmd " + std::to_string(cmd));
    complete_io(ior, 0, 0);
}

} // namespace tdort
