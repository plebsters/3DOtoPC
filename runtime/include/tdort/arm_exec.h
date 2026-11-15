// tdort/arm_exec.h - the single definition of ARMv3 instruction semantics.
//
// Both execution paths use exec():
//   * the interpreter calls it with a runtime instruction word;
//   * recompiled blocks call it with compile-time constants, so the compiler
//     folds the decode away and leaves only the operation itself.
// Because there is one implementation, recompiled code and interpreted code
// cannot disagree.
//
// exec() returns true when the instruction wrote r15 (control flow changed),
// in which case c->r[15] already holds the new pc.
#pragma once
#include "cpu.h"
#include "memory.h"
#include "arm_isa.h"

namespace tdort {

// Implemented by the runtime (hle.cpp). Return true if pc was changed.
bool on_swi(Cpu* c, uint32_t comment, uint32_t pc);
void on_undefined(Cpu* c, uint32_t insn, uint32_t pc);

TDORT_FORCEINLINE uint32_t ror32(uint32_t v, uint32_t s) {
    s &= 31;
    return s ? (v >> s) | (v << (32 - s)) : v;
}

TDORT_FORCEINLINE bool cond_pass(const Cpu* c, uint32_t cond) {
    switch (cond) {
    case 0x0: return c->z;
    case 0x1: return !c->z;
    case 0x2: return c->c;
    case 0x3: return !c->c;
    case 0x4: return c->n;
    case 0x5: return !c->n;
    case 0x6: return c->v;
    case 0x7: return !c->v;
    case 0x8: return c->c && !c->z;
    case 0x9: return !c->c || c->z;
    case 0xA: return c->n == c->v;
    case 0xB: return c->n != c->v;
    case 0xC: return !c->z && c->n == c->v;
    case 0xD: return c->z || c->n != c->v;
    case 0xE: return true;
    default:  return false;   // NV: never (ARMv3)
    }
}

// Register read with the ARM pipeline offset for r15.
TDORT_FORCEINLINE uint32_t rreg(const Cpu* c, uint32_t n, uint32_t pc, uint32_t pc_off = 8) {
    return n == 15 ? pc + pc_off : c->r[n];
}

TDORT_FORCEINLINE uint32_t shift_imm(uint32_t v, uint32_t type, uint32_t amt, uint32_t cin, uint32_t& cout) {
    switch (type) {
    case 0: // LSL
        if (amt == 0) { cout = cin; return v; }
        cout = (v >> (32 - amt)) & 1; return v << amt;
    case 1: // LSR (#0 encodes #32)
        if (amt == 0) { cout = v >> 31; return 0; }
        cout = (v >> (amt - 1)) & 1; return v >> amt;
    case 2: // ASR (#0 encodes #32)
        if (amt == 0) { cout = v >> 31; return uint32_t(int32_t(v) >> 31); }
        cout = (uint32_t(int32_t(v) >> (amt - 1))) & 1; return uint32_t(int32_t(v) >> amt);
    default: // ROR (#0 encodes RRX)
        if (amt == 0) { cout = v & 1; return (cin << 31) | (v >> 1); }
        cout = (v >> (amt - 1)) & 1; return ror32(v, amt);
    }
}

TDORT_FORCEINLINE uint32_t shift_reg(uint32_t v, uint32_t type, uint32_t amt, uint32_t cin, uint32_t& cout) {
    amt &= 0xFF;
    if (amt == 0) { cout = cin; return v; }
    switch (type) {
    case 0:
        if (amt < 32)  { cout = (v >> (32 - amt)) & 1; return v << amt; }
        cout = (amt == 32) ? (v & 1) : 0; return 0;
    case 1:
        if (amt < 32)  { cout = (v >> (amt - 1)) & 1; return v >> amt; }
        cout = (amt == 32) ? (v >> 31) : 0; return 0;
    case 2:
        if (amt < 32)  { cout = uint32_t(int32_t(v) >> (amt - 1)) & 1; return uint32_t(int32_t(v) >> amt); }
        cout = v >> 31; return uint32_t(int32_t(v) >> 31);
    default: {
        uint32_t s = amt & 31;
        if (s == 0) { cout = v >> 31; return v; }
        cout = (v >> (s - 1)) & 1; return ror32(v, s);
    }
    }
}

TDORT_FORCEINLINE uint32_t add_with_carry(uint32_t a, uint32_t b, uint32_t cin, uint32_t& cout, uint32_t& vout) {
    uint64_t wide = uint64_t(a) + uint64_t(b) + cin;
    uint32_t r = uint32_t(wide);
    cout = uint32_t(wide >> 32);
    vout = ((a ^ r) & (b ^ r)) >> 31;
    return r;
}

// Unaligned LDR: big-endian ARM rotates the aligned word so the addressed
// byte lands in bits 31..24. TODO: confirm against hardware test ROMs.
TDORT_FORCEINLINE uint32_t load_word_rot(Cpu* c, uint32_t addr) {
    uint32_t w = mem_r32(c, addr & ~3u);
    uint32_t k = addr & 3;
    return k ? ror32(w, 32 - 8 * k) : w;
}

TDORT_FORCEINLINE bool write_reg(Cpu* c, uint32_t rd, uint32_t val) {
    if (rd == 15) { c->r[15] = val & ~3u; return true; }
    c->r[rd] = val;
    return false;
}

TDORT_FORCEINLINE bool exec_dataproc(Cpu* c, uint32_t i, uint32_t pc) {
    const uint32_t op = (i >> 21) & 15, s = (i >> 20) & 1;
    const uint32_t rn = (i >> 16) & 15, rd = (i >> 12) & 15;
    uint32_t op2, sc = c->c;
    uint32_t pc_off = 8;
    if (i & 0x02000000) {
        uint32_t rot = ((i >> 8) & 15) * 2;
        op2 = ror32(i & 0xFF, rot);
        if (rot) sc = op2 >> 31;
    } else {
        const uint32_t rm = i & 15, type = (i >> 5) & 3;
        if (i & 0x10) {
            pc_off = 12;
            op2 = shift_reg(rreg(c, rm, pc, 12), type, c->r[(i >> 8) & 15], c->c, sc);
        } else {
            op2 = shift_imm(rreg(c, rm, pc), type, (i >> 7) & 31, c->c, sc);
        }
    }
    const uint32_t a = rreg(c, rn, pc, pc_off);
    uint32_t r = 0, cf = c->c, vf = c->v;
    bool logical = false;
    switch (op) {
    case 0x0: case 0x8: r = a & op2;  logical = true; break;           // AND / TST
    case 0x1: case 0x9: r = a ^ op2;  logical = true; break;           // EOR / TEQ
    case 0x2: case 0xA: r = add_with_carry(a, ~op2, 1, cf, vf); break; // SUB / CMP
    case 0x3:           r = add_with_carry(op2, ~a, 1, cf, vf); break; // RSB
    case 0x4: case 0xB: r = add_with_carry(a, op2, 0, cf, vf); break;  // ADD / CMN
    case 0x5:           r = add_with_carry(a, op2, c->c, cf, vf); break;   // ADC
    case 0x6:           r = add_with_carry(a, ~op2, c->c, cf, vf); break;  // SBC
    case 0x7:           r = add_with_carry(op2, ~a, c->c, cf, vf); break;  // RSC
    case 0xC:           r = a | op2;  logical = true; break;           // ORR
    case 0xD:           r = op2;      logical = true; break;           // MOV
    case 0xE:           r = a & ~op2; logical = true; break;           // BIC
    default:            r = ~op2;     logical = true; break;           // MVN
    }
    if (s) {
        if (rd == 15 && !(op >= 8 && op <= 11)) {
            // MOVS pc, ... restores CPSR from SPSR in privileged modes.
            // The HLE kernel keeps the game in user mode, so nothing to restore.
        } else {
            c->n = r >> 31; c->z = (r == 0);
            if (logical) c->c = sc; else { c->c = cf; c->v = vf; }
        }
    }
    if (op >= 8 && op <= 11) return false;
    return write_reg(c, rd, r);
}

TDORT_FORCEINLINE bool exec_mul(Cpu* c, uint32_t i) {
    const uint32_t rd = (i >> 16) & 15, rn = (i >> 12) & 15, rs = (i >> 8) & 15, rm = i & 15;
    if (i & 0x00800000) {   // UMULL / UMLAL / SMULL / SMLAL: RdHi = rd, RdLo = rn
        uint64_t r = (i & 0x00400000) ? uint64_t(int64_t(int32_t(c->r[rm])) * int64_t(int32_t(c->r[rs])))
                                      : uint64_t(c->r[rm]) * uint64_t(c->r[rs]);
        if (i & 0x00200000) r += (uint64_t(c->r[rd]) << 32) | c->r[rn];
        if (i & 0x00100000) { c->n = uint32_t(r >> 63); c->z = (r == 0); }
        if (rn != 15) c->r[rn] = uint32_t(r);
        if (rd != 15) c->r[rd] = uint32_t(r >> 32);
        return false;
    }
    uint32_t r = c->r[rm] * c->r[rs];
    if (i & 0x00200000) r += c->r[rn];
    if (i & 0x00100000) { c->n = r >> 31; c->z = (r == 0); }   // C is unpredictable: left unchanged
    if (rd != 15) c->r[rd] = r;
    return false;
}

TDORT_FORCEINLINE bool exec_swp(Cpu* c, uint32_t i) {
    const uint32_t rn = (i >> 16) & 15, rd = (i >> 12) & 15, rm = i & 15;
    const uint32_t addr = c->r[rn], src = c->r[rm];
    uint32_t tmp;
    if (i & 0x00400000) { tmp = mem_r8(c, addr); mem_w8(c, addr, uint8_t(src)); }
    else                { tmp = load_word_rot(c, addr); mem_w32(c, addr & ~3u, src); }
    return write_reg(c, rd, tmp);
}

TDORT_FORCEINLINE bool exec_single(Cpu* c, uint32_t i, uint32_t pc) {
    const bool P = i & (1u << 24), U = i & (1u << 23), B = i & (1u << 22);
    const bool W = i & (1u << 21), L = i & (1u << 20);
    const uint32_t rn = (i >> 16) & 15, rd = (i >> 12) & 15;
    uint32_t off;
    if (i & 0x02000000) {
        uint32_t dummy;
        off = shift_imm(rreg(c, i & 15, pc), (i >> 5) & 3, (i >> 7) & 31, c->c, dummy);
    } else {
        off = i & 0xFFF;
    }
    const uint32_t base = rreg(c, rn, pc);
    const uint32_t moved = U ? base + off : base - off;
    const uint32_t addr = P ? moved : base;
    const bool wb = !P || W;
    if (L) {
        uint32_t val = B ? mem_r8(c, addr) : load_word_rot(c, addr);
        if (wb && rn != 15) c->r[rn] = moved;
        return write_reg(c, rd, val);          // a load into the base wins over writeback
    }
    uint32_t val = rreg(c, rd, pc, 12);
    if (B) mem_w8(c, addr, uint8_t(val)); else mem_w32(c, addr & ~3u, val);
    if (wb && rn != 15) c->r[rn] = moved;
    return false;
}

TDORT_FORCEINLINE bool exec_half(Cpu* c, uint32_t i, uint32_t pc) {
    const bool P = i & (1u << 24), U = i & (1u << 23), W = i & (1u << 21), L = i & (1u << 20);
    const uint32_t rn = (i >> 16) & 15, rd = (i >> 12) & 15, sh = (i >> 5) & 3;
    const uint32_t off = (i & (1u << 22)) ? (((i >> 4) & 0xF0) | (i & 0xF)) : c->r[i & 15];
    const uint32_t base = rreg(c, rn, pc);
    const uint32_t moved = U ? base + off : base - off;
    const uint32_t addr = P ? moved : base;
    const bool wb = !P || W;
    if (L) {
        uint32_t val;
        if (sh == 1)      val = mem_r16(c, addr & ~1u);
        else if (sh == 2) val = uint32_t(int32_t(int8_t(mem_r8(c, addr))));
        else              val = uint32_t(int32_t(int16_t(mem_r16(c, addr & ~1u))));
        if (wb && rn != 15) c->r[rn] = moved;
        return write_reg(c, rd, val);
    }
    if (sh != 1) { on_undefined(c, i, pc); return false; }   // LDRD/STRD are ARMv5
    mem_w16(c, addr & ~1u, uint16_t(rreg(c, rd, pc, 12)));
    if (wb && rn != 15) c->r[rn] = moved;
    return false;
}

TDORT_FORCEINLINE uint32_t popcount16(uint32_t x) {
    x &= 0xFFFF; uint32_t n = 0;
    while (x) { x &= x - 1; ++n; }
    return n;
}

TDORT_FORCEINLINE bool exec_block(Cpu* c, uint32_t i, uint32_t pc) {
    const bool P = i & (1u << 24), U = i & (1u << 23), W = i & (1u << 21), L = i & (1u << 20);
    const uint32_t rn = (i >> 16) & 15, list = i & 0xFFFF;
    // S bit (user bank transfer / CPSR restore) is ignored: game runs in user mode.
    if (list == 0) return false;
    const uint32_t n = popcount16(list);
    const uint32_t base = c->r[rn];
    uint32_t addr;
    if (U) addr = P ? base + 4 : base;
    else   addr = P ? base - 4 * n : base - 4 * n + 4;
    const uint32_t new_base = U ? base + 4 * n : base - 4 * n;
    addr &= ~3u;
    bool pc_written = false;
    if (L) {
        if (W) c->r[rn] = new_base;
        for (uint32_t r = 0; r < 16; ++r) {
            if (!(list & (1u << r))) continue;
            uint32_t v = mem_r32(c, addr); addr += 4;
            if (r == 15) { c->r[15] = v & ~3u; pc_written = true; }
            else c->r[r] = v;
        }
    } else {
        uint32_t first = 0;
        while (!(list & (1u << first))) ++first;
        for (uint32_t r = 0; r < 16; ++r) {
            if (!(list & (1u << r))) continue;
            uint32_t v;
            if (r == 15) v = pc + 12;
            else if (r == rn && W && r != first) v = new_base;   // ARM6/7: base stored updated unless first
            else v = c->r[r];
            mem_w32(c, addr, v); addr += 4;
        }
        if (W) c->r[rn] = new_base;
    }
    return pc_written;
}

TDORT_FORCEINLINE bool exec(Cpu* c, uint32_t i, uint32_t pc) {
    if (!cond_pass(c, cond_of(i))) return false;
    switch (classify(i)) {
    case Op::DataProc:   return exec_dataproc(c, i, pc);
    case Op::Mul:        return exec_mul(c, i);
    case Op::Swp:        return exec_swp(c, i);
    case Op::Mrs:        c->r[(i >> 12) & 15] = c->cpsr(); return false;
    case Op::Msr: {
        uint32_t val = (i & 0x02000000) ? ror32(i & 0xFF, ((i >> 8) & 15) * 2) : c->r[i & 15];
        // SPSR writes (bit 22) are ignored in user mode.
        if (!(i & 0x00400000)) c->set_cpsr(val, (i >> 19) & 1, false);
        return false;
    }
    case Op::SingleXfer: return exec_single(c, i, pc);
    case Op::HalfXfer:   return exec_half(c, i, pc);
    case Op::BlockXfer:  return exec_block(c, i, pc);
    case Op::Branch:
        if (i & 0x01000000) c->r[14] = pc + 4;
        c->r[15] = branch_target(i, pc);
        return true;
    case Op::Swi:        return on_swi(c, i & 0x00FFFFFF, pc);
    default:             on_undefined(c, i, pc); return false;
    }
}

} // namespace tdort
