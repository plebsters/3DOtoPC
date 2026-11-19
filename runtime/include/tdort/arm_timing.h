#pragma once
// How long an ARM60 instruction takes, in 12.5 MHz clocks.
//
// The ARM60 has no cache and runs from DRAM. A sequential memory cycle (S)
// costs 1 clock, a non-sequential one (N) 2 clocks (Cpu::n_clocks, TDORT_N_CLOCKS)
// and an internal cycle (I)
// 1 clock. The counts per instruction class come from the ARM6 datasheet:
//
//   data processing          1S          (+1I with a register-specified shift,
//                                         2S+1N when it writes the PC)
//   LDR                      1S+1N+1I    (+2S+1N when it loads the PC)
//   STR                      2N
//   LDM  (n registers)       nS+1N+1I    (+2S+1N when it loads the PC)
//   STM  (n registers)       (n-1)S+2N
//   SWP                      1S+2N+1I
//   B, BL                    2S+1N
//   MUL / MLA                1S+mI (+1I for MLA); m depends on the
//                            multiplier value, counted as 4 here
//   SWI                      2S+1N
//   coprocessor / undefined  1S
//   condition false          1S
//
// The translator adds these up per block when it writes the C++; the
// interpreter adds them per instruction. Both use this table, so a game
// runs at the same speed whichever way its code runs.
#include <cstdint>

namespace tdort {

// An instruction's cost split into clocks that do not depend on the memory
// (S and I cycles, 1 clock each) and the number of N cycles, whose cost the
// runtime sets (Cpu::n_clocks, 2 by default; TDORT_N_CLOCKS changes it).
struct ArmCost { uint32_t si = 0, n = 0; };

constexpr ArmCost operator+(ArmCost a, ArmCost b) { return {a.si + b.si, a.n + b.n}; }

constexpr uint32_t reg_count(uint32_t v) {
    v &= 0xFFFF; uint32_t n = 0;
    while (v) { n += v & 1; v >>= 1; }
    return n;
}

// Cost of one instruction whose condition passes.
constexpr ArmCost arm_cost(uint32_t i) {
    const uint32_t rd = (i >> 12) & 15;
    switch ((i >> 25) & 7) {
    case 0:
        if ((i & 0x0FC000F0) == 0x00000090)                 // MUL, MLA: 1S + mI (+1I)
            return {1 + 4 + ((i & 0x00200000) ? 1u : 0u), 0};
        if ((i & 0x0FB00FF0) == 0x01000090)                 // SWP: 1S+2N+1I
            return {2, 2};
        if ((i & 0x0E000090) == 0x00000090)                 // halfword forms (not on ARM60)
            return {1, 0};
        if ((i & 0x0FBF0FFF) == 0x010F0000 || (i & 0x0DB0F000) == 0x0120F000)   // MRS, MSR
            return {1, 0};
        [[fallthrough]];
    case 1: {                                               // data processing: 1S (+1I), PC: 2S+1N
        const uint32_t op = (i >> 21) & 15;
        const bool test = op >= 8 && op <= 11;
        const uint32_t shift = (!(i & 0x02000000) && (i & 0x10)) ? 1u : 0u;
        if (!test && rd == 15) return {2 + shift, 1};
        return {1 + shift, 0};
    }
    case 2: case 3:                                         // LDR 1S+1N+1I (+2S+1N), STR 2N
        if (i & 0x00100000) return rd == 15 ? ArmCost{4, 2} : ArmCost{2, 1};
        return {0, 2};
    case 4: {                                               // LDM nS+1N+1I (+2S+1N), STM (n-1)S+2N
        const uint32_t n = reg_count(i) ? reg_count(i) : 1;
        if (i & 0x00100000) return (i & 0x8000) ? ArmCost{n + 3, 2} : ArmCost{n + 1, 1};
        return {n - 1, 2};
    }
    case 5:                                                 // B, BL: 2S+1N
        return {2, 1};
    case 6:
        return {1, 0};                                      // coprocessor transfer (none fitted)
    default:
        return ((i >> 24) & 15) == 15 ? ArmCost{2, 1} : ArmCost{1, 0};   // SWI: 2S+1N
    }
}

// Clocks for a cost with N cycles of n_clocks each.
constexpr uint32_t arm_clocks(ArmCost c, uint32_t n_clocks) { return c.si + c.n * n_clocks; }

// An instruction whose condition failed: 1S.
constexpr ArmCost arm_cost_skipped() { return {1, 0}; }

}  // namespace tdort
