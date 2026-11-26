// tdort/memory.h - 3DO guest address space.
//
// Default map (configurable in MemoryConfig):
//   0x00000000  DRAM   2 MB
//   0x00200000  VRAM   1 MB
//   0x03000000+ hardware / ROM  -> io callbacks (Madam, Clio, ...)
//   0x0F000000  HLE trap region: jumping here calls a host handler
//
// Guest memory is stored in guest (big-endian) byte order.
#pragma once
#include <cstdint>
#include <cstring>
#include <vector>
#include "cpu.h"

namespace tdort {

constexpr uint32_t kDramBase   = 0x00000000;
constexpr uint32_t kDramSize   = 2u * 1024 * 1024;
constexpr uint32_t kVramBase   = 0x00200000;
constexpr uint32_t kVramSize   = 1u * 1024 * 1024;
constexpr uint32_t kRamEnd     = kVramBase + kVramSize;   // DRAM+VRAM contiguous
// Fixed regions at the top of DRAM owned by the runtime.
constexpr uint32_t kHleHeapSize = 0x10000;                     // OS structures (folios, items)
constexpr uint32_t kHleHeapBase = kDramSize - kHleHeapSize;
constexpr uint32_t kStackSize   = 0x10000;                     // main task stack
constexpr uint32_t kStackTop    = kHleHeapBase;
constexpr uint32_t kTrapBase   = 0x0F000000;
constexpr uint32_t kTrapSize   = 0x00100000;

// Recompiled code is tracked in 64-byte granules so a write that lands on
// code (self-modifying code, overlays) disables only the blocks it touches.
constexpr uint32_t kGranuleShift = 6;
constexpr uint32_t kGranuleCount = kRamEnd >> kGranuleShift;

using IoRead  = uint32_t (*)(void* user, uint32_t addr, int size);
using IoWrite = void     (*)(void* user, uint32_t addr, uint32_t val, int size);

struct Memory {
    std::vector<uint8_t> ram;                 // DRAM + VRAM, kRamEnd bytes
    std::vector<uint8_t> code_granule;        // 1 = granule holds recompiled code
    std::vector<uint8_t> granule_dirty;       // 1 = recompiled code here was overwritten
    // Who overwrote it (diagnosis): the guest pc, or 0xFFFFFFFF and the name
    // of the OS call that was running (a disc read, memset, ...).
    std::vector<uint32_t> dirty_by;
    std::vector<const char*> dirty_call;
    const char* host_call = nullptr;          // set by the runtime around each OS call
    IoRead  io_read  = nullptr;
    IoWrite io_write = nullptr;
    void*   io_user  = nullptr;

    Memory() : ram(kRamEnd, 0), code_granule(kGranuleCount, 0), granule_dirty(kGranuleCount, 0),
               dirty_by(kGranuleCount, 0), dirty_call(kGranuleCount, nullptr) {}

    void invalidate(uint32_t addr, uint32_t len) {
        if (addr >= kRamEnd || len == 0) return;
        uint32_t end = addr + len > kRamEnd ? kRamEnd : addr + len;
        for (uint32_t g = addr >> kGranuleShift; g <= (end - 1) >> kGranuleShift; ++g)
            if (code_granule[g] && !granule_dirty[g]) { granule_dirty[g] = 1; dirty_by[g] = 0xFFFFFFFFu; dirty_call[g] = host_call; }
    }
    // Debugging aid: TDORT_WRITE_WATCH=<hex addr>[,<hex len>] marks those
    // granules (code_granule = 2) so guest stores there take the slow path,
    // and logs each store into the range with its pc (exact under --interp).
    uint32_t watch_lo = 0, watch_hi = 0, watch_hits = 0;
    void watch_store(uint32_t addr, uint32_t v, int size, uint32_t pc);
    void mark_cpu_write(uint32_t g, uint32_t pc, uint32_t addr = 0, uint32_t v = 0, int size = 0) {
        if (code_granule[g] == 2) { if (size) watch_store(addr, v, size, pc); return; }
        if (!granule_dirty[g]) { granule_dirty[g] = 1; dirty_by[g] = pc; dirty_call[g] = nullptr; }
    }
    // Host-side bulk copy into guest memory (file loads, DMA). Invalidates code.
    void write_block(uint32_t addr, const void* src, uint32_t len) {
        if (addr >= kRamEnd || addr + len > kRamEnd) return;
        std::memcpy(&ram[addr], src, len);
        invalidate(addr, len);
    }
};

TDORT_FORCEINLINE uint32_t load_be32(const uint8_t* p) {
    return (uint32_t(p[0]) << 24) | (uint32_t(p[1]) << 16) | (uint32_t(p[2]) << 8) | p[3];
}
TDORT_FORCEINLINE void store_be32(uint8_t* p, uint32_t v) {
    p[0] = uint8_t(v >> 24); p[1] = uint8_t(v >> 16); p[2] = uint8_t(v >> 8); p[3] = uint8_t(v);
}

// Word access; addr must already be word aligned.
TDORT_FORCEINLINE uint32_t mem_r32(Cpu* c, uint32_t addr) {
    Memory* m = c->mem;
    if (addr < kRamEnd) return load_be32(&m->ram[addr]);
    return m->io_read ? m->io_read(m->io_user, addr, 4) : 0;
}
TDORT_FORCEINLINE uint8_t mem_r8(Cpu* c, uint32_t addr) {
    Memory* m = c->mem;
    if (addr < kRamEnd) return m->ram[addr];
    return m->io_read ? uint8_t(m->io_read(m->io_user, addr, 1)) : 0;
}
TDORT_FORCEINLINE uint16_t mem_r16(Cpu* c, uint32_t addr) {
    return uint16_t((mem_r8(c, addr) << 8) | mem_r8(c, addr + 1));
}
TDORT_FORCEINLINE void mem_w32(Cpu* c, uint32_t addr, uint32_t v) {
    Memory* m = c->mem;
    if (addr < kRamEnd) {
        store_be32(&m->ram[addr], v);
        uint32_t g = addr >> kGranuleShift;
        if (m->code_granule[g]) m->mark_cpu_write(g, c->r[15], addr, v, 4);
        return;
    }
    if (m->io_write) m->io_write(m->io_user, addr, v, 4);
}
TDORT_FORCEINLINE void mem_w8(Cpu* c, uint32_t addr, uint8_t v) {
    Memory* m = c->mem;
    if (addr < kRamEnd) {
        m->ram[addr] = v;
        uint32_t g = addr >> kGranuleShift;
        if (m->code_granule[g]) m->mark_cpu_write(g, c->r[15], addr, v, 1);
        return;
    }
    if (m->io_write) m->io_write(m->io_user, addr, v, 1);
}

TDORT_FORCEINLINE void mem_w16(Cpu* c, uint32_t addr, uint16_t v) {
    mem_w8(c, addr, uint8_t(v >> 8));
    mem_w8(c, addr + 1, uint8_t(v));
}

} // namespace tdort
