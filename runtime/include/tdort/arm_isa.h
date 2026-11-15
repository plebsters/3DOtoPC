// tdort/arm_isa.h - ARMv3 (ARM60) instruction classification.
// Shared by the converter's analyzer and the runtime, so both agree on what
// every instruction word means.
#pragma once
#include <cstdint>

namespace tdort {

enum class Op : uint8_t {
    DataProc, Mul, Swp, Mrs, Msr, SingleXfer, BlockXfer, Branch, Swi, Coproc, HalfXfer, Undefined
};

constexpr Op classify(uint32_t i) {
    switch ((i >> 26) & 3) {
    case 0:
        if ((i & 0x0FC000F0) == 0x00000090) return Op::Mul;
        // Long multiplies (ARMv3M, not on the ARM60): accepted for compiler-built tests.
        if ((i & 0x0F8000F0) == 0x00800090) return Op::Mul;
        if ((i & 0x0FB00FF0) == 0x01000090) return Op::Swp;
        if ((i & 0x0FBF0FFF) == 0x010F0000) return Op::Mrs;
        if ((i & 0x0DB0F000) == 0x0120F000) return Op::Msr;
        // ARMv4 halfword / signed transfers. Undefined on a real ARM60, so no
        // 3DO game uses them; supported so test programs can be built with
        // modern compilers (-march=armv4).
        if ((i & 0x0E000090) == 0x00000090) return (i & 0x60) ? Op::HalfXfer : Op::Undefined;
        return Op::DataProc;
    case 1:
        if ((i & 0x02000010) == 0x02000010) return Op::Undefined;
        return Op::SingleXfer;
    case 2:
        return (i & 0x02000000) ? Op::Branch : Op::BlockXfer;
    default:
        return ((i & 0x0F000000) == 0x0F000000) ? Op::Swi : Op::Coproc;
    }
}

constexpr uint32_t cond_of(uint32_t i) { return i >> 28; }
constexpr bool is_test_op(uint32_t i) { uint32_t op = (i >> 21) & 15; return op >= 8 && op <= 11; }

// True if executing the instruction may write r15 (i.e. it is control flow).
constexpr bool writes_pc(uint32_t i) {
    switch (classify(i)) {
    case Op::DataProc:   return !is_test_op(i) && ((i >> 12) & 15) == 15;
    case Op::SingleXfer:
    case Op::HalfXfer:   return (i & 0x00100000) && ((i >> 12) & 15) == 15;
    case Op::BlockXfer:  return (i & 0x00100000) && (i & 0x8000);
    case Op::Branch:     return true;
    default:             return false;
    }
}

constexpr bool is_bl(uint32_t i) { return classify(i) == Op::Branch && (i & 0x01000000); }

constexpr uint32_t branch_target(uint32_t i, uint32_t pc) {
    int32_t off = int32_t(i << 8) >> 6;     // sign-extend imm24, *4
    return pc + 8 + uint32_t(off);
}

} // namespace tdort
