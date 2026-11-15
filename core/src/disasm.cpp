// disasm.cpp - ARMv3 disassembler (for listings and logs).
#include "tdo/recomp.h"
#include "tdort/arm_isa.h"
#include <cstdio>

namespace tdo {

static const char* kCond[16] = {"eq","ne","cs","cc","mi","pl","vs","vc","hi","ls","ge","lt","gt","le","","nv"};
static const char* kDp[16] = {"and","eor","sub","rsb","add","adc","sbc","rsc","tst","teq","cmp","cmn","orr","mov","bic","mvn"};
static const char* kShift[4] = {"lsl","lsr","asr","ror"};

static std::string reg(uint32_t r) {
    static const char* names[16] = {"r0","r1","r2","r3","r4","r5","r6","r7","r8","r9","r10","r11","r12","sp","lr","pc"};
    return names[r & 15];
}
static std::string hex(uint32_t v) { char b[16]; std::snprintf(b, sizeof b, "0x%X", v); return b; }

static std::string operand2(uint32_t i) {
    if (i & 0x02000000) {
        uint32_t rot = ((i >> 8) & 15) * 2, v = i & 0xFF;
        v = rot ? (v >> rot) | (v << (32 - rot)) : v;
        return "#" + hex(v);
    }
    std::string s = reg(i & 15);
    uint32_t type = (i >> 5) & 3;
    if (i & 0x10) return s + ", " + kShift[type] + " " + reg((i >> 8) & 15);
    uint32_t amt = (i >> 7) & 31;
    if (amt == 0) {
        if (type == 0) return s;
        if (type == 3) return s + ", rrx";
        amt = 32;
    }
    return s + ", " + kShift[type] + " #" + std::to_string(amt);
}

std::string disasm(uint32_t i, uint32_t pc) {
    using tdort::Op;
    const std::string c = kCond[i >> 28];
    switch (tdort::classify(i)) {
    case Op::DataProc: {
        uint32_t op = (i >> 21) & 15;
        bool s = (i >> 20) & 1;
        std::string m = std::string(kDp[op]) + c + ((s && !(op >= 8 && op <= 11)) ? "s" : "");
        if (op >= 8 && op <= 11) return m + " " + reg((i >> 16) & 15) + ", " + operand2(i);
        if (op == 13 || op == 15) return m + " " + reg((i >> 12) & 15) + ", " + operand2(i);
        return m + " " + reg((i >> 12) & 15) + ", " + reg((i >> 16) & 15) + ", " + operand2(i);
    }
    case Op::Mul: {
        bool a = i & 0x00200000;
        std::string m = std::string(a ? "mla" : "mul") + c + ((i & 0x00100000) ? "s" : "");
        std::string s = m + " " + reg((i >> 16) & 15) + ", " + reg(i & 15) + ", " + reg((i >> 8) & 15);
        return a ? s + ", " + reg((i >> 12) & 15) : s;
    }
    case Op::Swp:
        return std::string("swp") + c + ((i & 0x00400000) ? "b " : " ") + reg((i >> 12) & 15) + ", " +
               reg(i & 15) + ", [" + reg((i >> 16) & 15) + "]";
    case Op::Mrs: return "mrs" + c + " " + reg((i >> 12) & 15) + ", " + ((i & 0x00400000) ? "spsr" : "cpsr");
    case Op::Msr: return "msr" + c + " " + ((i & 0x00400000) ? "spsr" : "cpsr") + ", " + operand2(i);
    case Op::SingleXfer: {
        bool L = i & 0x00100000, B = i & 0x00400000, P = i & 0x01000000, U = i & 0x00800000, W = i & 0x00200000;
        std::string m = std::string(L ? "ldr" : "str") + c + (B ? "b" : "");
        std::string off;
        if (i & 0x02000000) {
            uint32_t r = i & ~0x10u & ~0x02000000u;   // reuse operand2 shift formatter on Rm
            off = std::string(U ? "" : "-") + operand2(r & ~0x02000000u);
        } else {
            off = "#" + std::string(U ? "" : "-") + hex(i & 0xFFF);
        }
        std::string base = reg((i >> 16) & 15);
        std::string s = m + " " + reg((i >> 12) & 15) + ", ";
        if (P) s += "[" + base + ", " + off + "]" + (W ? "!" : "");
        else   s += "[" + base + "], " + off;
        if (((i >> 16) & 15) == 15 && !(i & 0x02000000) && P) {
            uint32_t addr = pc + 8 + (U ? (i & 0xFFF) : uint32_t(-int32_t(i & 0xFFF)));
            s += "    ; =" + hex(addr);
        }
        return s;
    }
    case Op::BlockXfer: {
        bool L = i & 0x00100000;
        static const char* modes[4] = {"da", "ia", "db", "ib"};
        std::string m = std::string(L ? "ldm" : "stm") + c + modes[((i >> 23) & 1) | (((i >> 24) & 1) << 1)];
        std::string s = m + " " + reg((i >> 16) & 15) + ((i & 0x00200000) ? "!" : "") + ", {";
        bool first = true;
        for (int r = 0; r < 16; ++r) if (i & (1u << r)) { s += (first ? "" : ", ") + reg(r); first = false; }
        return s + "}" + ((i & 0x00400000) ? "^" : "");
    }
    case Op::Branch:
        return std::string(tdort::is_bl(i) ? "bl" : "b") + c + " " + hex(tdort::branch_target(i, pc));
    case Op::Swi: return "swi" + c + " " + hex(i & 0xFFFFFF);
    case Op::Coproc: return "cdp/ldc/stc " + hex(i);
    default: return ".word " + hex(i);
    }
}

} // namespace tdo
