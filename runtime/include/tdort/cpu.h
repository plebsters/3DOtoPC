// tdort/cpu.h - ARM60 (ARMv3, big-endian, 32-bit mode) guest CPU state.
#pragma once
#include <cstdint>
#include <cstddef>

#if defined(_MSC_VER)
#define TDORT_FORCEINLINE __forceinline
#else
#define TDORT_FORCEINLINE inline __attribute__((always_inline))
#endif

namespace tdort {

struct Memory;
struct Runtime;

struct Cpu {
    // r[15] holds the address of the NEXT instruction to execute between
    // instructions. While executing an instruction, reads of r15 return pc+8
    // (or pc+12 for register-specified shifts / STR / STM), as on hardware.
    uint32_t r[16] = {};

    // Flags kept unpacked (0 or 1) so generated code can test them cheaply.
    uint32_t n = 0, z = 0, c = 0, v = 0;
    uint32_t ctl = 0x10;   // CPSR bits 7..0 (I, F, mode). 0x10 = user32.

    uint64_t cycles = 0;
    bool     halted = false;

    uint32_t mod_base = 0;   // load address of the module whose translated block is running

    Memory*  mem = nullptr;
    Runtime* rt  = nullptr;

    uint32_t cpsr() const {
        return (n << 31) | (z << 30) | (c << 29) | (v << 28) | (ctl & 0xFF);
    }
    void set_cpsr(uint32_t val, bool flags, bool control) {
        if (flags) {
            n = (val >> 31) & 1; z = (val >> 30) & 1;
            c = (val >> 29) & 1; v = (val >> 28) & 1;
        }
        // Mode switches are not banked yet: the HLE kernel keeps the game in
        // user mode. Control writes are recorded so they can be inspected.
        if (control) ctl = val & 0xFF;
    }
};

} // namespace tdort
