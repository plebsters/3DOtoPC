// test_arm.cpp - unit tests for the shared ARM semantics and the Opera reader.
#include "tdort/arm_exec.h"
#include "tdort/runtime.h"
#include "tdo/opera_fs.h"
#include "tdo/recomp.h"
#include <cstdio>

// The runtime library expects these from generated code.
extern "C" const tdort::RecompEntry tdort_recomp_table[] = {{0, 0, nullptr}};
extern "C" const uint32_t tdort_recomp_count = 0;
extern "C" const char tdort_image_hash[] = "";
extern "C" const char tdort_game_name[] = "test";
extern "C" const uint32_t tdort_image_base = 0;

static int failures = 0;
#define CHECK(x) do { if (!(x)) { std::printf("FAIL %s:%d: %s\n", __FILE__, __LINE__, #x); ++failures; } } while (0)

using namespace tdort;

int main(int argc, char** argv) {
    Runtime rt{RuntimeConfig{}};
    Cpu& c = rt.cpu;

    // ADDS overflow / carry
    c.r[1] = 0x7FFFFFFF; c.r[2] = 1;
    exec(&c, 0xE0910002, 0x1000);            // adds r0, r1, r2
    CHECK(c.r[0] == 0x80000000 && c.n == 1 && c.v == 1 && c.c == 0 && c.z == 0);

    // SUBS borrow: 0 - 1
    c.r[1] = 0; c.r[2] = 1;
    exec(&c, 0xE0510002, 0x1000);            // subs r0, r1, r2
    CHECK(c.r[0] == 0xFFFFFFFF && c.c == 0 && c.n == 1);

    // CMP equal sets Z and C
    c.r[1] = 5;
    exec(&c, 0xE3510005, 0x1000);            // cmp r1, #5
    CHECK(c.z == 1 && c.c == 1);

    // MOVS with LSR #32 encoding (lsr #0): carry = bit31, result 0
    c.r[1] = 0x80000000;
    exec(&c, 0xE1B00021, 0x1000);            // movs r0, r1, lsr #32
    CHECK(c.r[0] == 0 && c.c == 1 && c.z == 1);

    // RRX
    c.c = 1; c.r[1] = 2;
    exec(&c, 0xE1B00061, 0x1000);            // movs r0, r1, rrx
    CHECK(c.r[0] == 0x80000001 && c.c == 0);

    // Reading pc gives pc+8
    exec(&c, 0xE1A0000F, 0x2000);            // mov r0, pc
    CHECK(c.r[0] == 0x2008);

    // Branch / BL
    CHECK(exec(&c, 0xEB000002, 0x3000));     // bl +2 words
    CHECK(c.r[15] == 0x3010 && c.r[14] == 0x3004);

    // Conditional not taken
    c.z = 0;
    CHECK(!exec(&c, 0x0A000010, 0x3000));    // beq (not taken)

    // STMDB / LDMIA round trip with writeback
    c.r[13] = 0x8000; c.r[4] = 0x11111111; c.r[5] = 0x22222222; c.r[14] = 0x4444;
    exec(&c, 0xE92D4030, 0x4000);            // stmfd sp!, {r4, r5, lr}
    CHECK(c.r[13] == 0x8000 - 12);
    CHECK(mem_r32(&c, 0x8000 - 12) == 0x11111111 && mem_r32(&c, 0x8000 - 4) == 0x4444);
    c.r[4] = c.r[5] = 0;
    CHECK(exec(&c, 0xE8BD8030, 0x4004));     // ldmfd sp!, {r4, r5, pc}
    CHECK(c.r[4] == 0x11111111 && c.r[5] == 0x22222222 && c.r[15] == 0x4444 && c.r[13] == 0x8000);

    // Byte store/load, big-endian word layout
    c.r[1] = 0x9000; c.r[0] = 0xAABBCCDD;
    exec(&c, 0xE5810000, 0x5000);            // str r0, [r1]
    CHECK(mem_r8(&c, 0x9000) == 0xAA && mem_r8(&c, 0x9003) == 0xDD);
    exec(&c, 0xE5D12001, 0x5004);            // ldrb r2, [r1, #1]
    CHECK(c.r[2] == 0xBB);

    // Post-indexed LDR with writeback
    exec(&c, 0xE4913004, 0x5008);            // ldr r3, [r1], #4
    CHECK(c.r[3] == 0xAABBCCDD && c.r[1] == 0x9004);

    // MLA
    c.r[1] = 6; c.r[2] = 7; c.r[3] = 1;
    exec(&c, 0xE0203291, 0x6000);            // mla r0, r1, r2, r3
    CHECK(c.r[0] == 43);

    // Write tracking marks recompiled code dirty
    rt.mem.code_granule[0x9000 >> kGranuleShift] = 1;
    c.r[1] = 0x9000;
    exec(&c, 0xE5810000, 0x5000);
    CHECK(rt.mem.granule_dirty[0x9000 >> kGranuleShift] == 1);

    // Disassembler smoke test
    CHECK(tdo::disasm(0xE92D4010, 0) == "stmdb sp!, {r4, lr}");
    CHECK(tdo::disasm(0x1AFFFFFB, 0x98) == "bne 0x8C");

    // Opera reader on the synthetic disc
    if (argc > 1) {
        tdo::OperaDisc d; std::string err;
        CHECK(d.open(argv[1], err));
        CHECK(d.volume().label == "TESTDISC");
        CHECK(d.find("launchme") != nullptr);
        CHECK(d.find("/Docs/readme.txt") != nullptr);
        std::vector<uint8_t> data;
        if (auto* e = d.find("/Docs/readme.txt")) { CHECK(d.read_file(*e, data, err)); CHECK(data.size() == 30); }   // "synthetic 3DO to PC test disc\n"
    }

    std::printf("%s (%d failures)\n", failures ? "FAILED" : "all tests passed", failures);
    return failures ? 1 : 0;
}
