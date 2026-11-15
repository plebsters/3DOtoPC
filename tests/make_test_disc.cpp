// make_test_disc.cpp - writes a tiny synthetic Opera disc image whose LaunchMe
// is a hand-assembled ARM60 program. Used by the end-to-end test; contains no
// game data.
//
// Program: sum 1..100 (=5050), call func: r0 = r0*3 + (r0>>2) (=16412),
// addgt 7 if > 0x4000 (=16419), store/load a byte and add it (=16454),
// return through an indirect `mov pc, r5` to code the static analyzer cannot
// see, so that part must run in the interpreter. Exit via the AIF exit SWI.
#include <cstdint>
#include <cstdio>
#include <cstring>
#include <string>
#include <vector>

static void put32(std::vector<uint8_t>& b, size_t off, uint32_t v) {
    b[off] = uint8_t(v >> 24); b[off + 1] = uint8_t(v >> 16); b[off + 2] = uint8_t(v >> 8); b[off + 3] = uint8_t(v);
}

static std::vector<uint8_t> make_aif() {
    std::vector<uint32_t> w(53, 0);
    const uint32_t base = 0x00010000;
    w[0] = 0xE1A00000; w[1] = 0xE1A00000; w[2] = 0xE1A00000;
    w[3] = 0xEB00001B;           // bl entry: offset = 32 - 3 - 2 = 27 words
    w[4] = 0xEF0000FF;           // swi exit
    w[10] = base;                // image base
    w[12] = 32;                  // address mode
    // entry
    w[32] = 0xE92D4010;          // stmfd sp!, {r4, lr}
    w[33] = 0xE3A00000;          // mov r0, #0
    w[34] = 0xE3A01001;          // mov r1, #1
    w[35] = 0xE0800001;          // loop: add r0, r0, r1
    w[36] = 0xE2811001;          // add r1, r1, #1
    w[37] = 0xE3510065;          // cmp r1, #101
    w[38] = 0x1AFFFFFB;          // bne loop
    w[39] = 0xEB000003;          // bl func
    w[40] = 0xE28F5004;          // add r5, pc, #4      -> index 43
    w[41] = 0xE1A0F005;          // mov pc, r5          (indirect)
    w[42] = 0xE3A00000;          // mov r0, #0          (skipped)
    w[43] = 0xE8BD8010;          // ldmfd sp!, {r4, pc}
    // func
    w[44] = 0xE3A02003;          // mov r2, #3
    w[45] = 0xE0030290;          // mul r3, r0, r2
    w[46] = 0xE0830120;          // add r0, r3, r0, lsr #2
    w[47] = 0xE3500901;          // cmp r0, #0x4000
    w[48] = 0xC2800007;          // addgt r0, r0, #7
    w[49] = 0xE54D0001;          // strb r0, [sp, #-1]
    w[50] = 0xE55D6001;          // ldrb r6, [sp, #-1]
    w[51] = 0xE0800006;          // add r0, r0, r6
    w[52] = 0xE1A0F00E;          // mov pc, lr
    w[5] = uint32_t(w.size() * 4);   // ro size (includes header)
    std::vector<uint8_t> b(w.size() * 4);
    for (size_t i = 0; i < w.size(); ++i) put32(b, i * 4, w[i]);
    return b;
}

// Directory entry: flags id type[4] bsize bytes blocks burst gap name[32] last avatars[]
static size_t put_entry(std::vector<uint8_t>& blk, size_t off, uint32_t flags, uint32_t id, const char* type,
                        uint32_t bytes, uint32_t blocks, const char* name, uint32_t avatar) {
    put32(blk, off, flags); put32(blk, off + 4, id);
    std::memcpy(&blk[off + 8], type, 4);
    put32(blk, off + 12, 2048); put32(blk, off + 16, bytes); put32(blk, off + 20, blocks);
    put32(blk, off + 24, 1); put32(blk, off + 28, 0);
    std::strncpy(reinterpret_cast<char*>(&blk[off + 32]), name, 31);
    put32(blk, off + 64, 0); put32(blk, off + 68, avatar);
    return off + 72;
}

struct Ent { std::string name; bool dir; std::vector<uint8_t> data; std::vector<Ent> kids; uint32_t block = 0, blocks = 0; };

static uint32_t next_id = 2;

// Assigns blocks depth-first: a directory's own block, then its contents.
static void layout(Ent& e, uint32_t& next) {
    if (e.dir) { e.block = next++; e.blocks = 1; for (auto& k : e.kids) layout(k, next); }
    else { e.block = next; e.blocks = uint32_t((e.data.size() + 2047) / 2048); if (!e.blocks) e.blocks = 1; next += e.blocks; }
}

static void emit(const Ent& e, std::vector<uint8_t>& img) {
    uint8_t* dst = &img[size_t(e.block) * 2048];
    if (!e.dir) { std::memcpy(dst, e.data.data(), e.data.size()); return; }
    std::vector<uint8_t> blk(2048, 0);
    size_t off = 0x14;
    for (size_t i = 0; i < e.kids.size(); ++i) {
        const Ent& k = e.kids[i];
        uint32_t flags = (k.dir ? 0x07 : 0x02) | (i + 1 == e.kids.size() ? 0x80000000u : 0);
        off = put_entry(blk, off, flags, next_id++, k.dir ? "*dir" : "    ",
                        k.dir ? 2048 : uint32_t(k.data.size()), k.blocks, k.name.c_str(), k.block);
    }
    put32(blk, 0, 0xFFFFFFFF); put32(blk, 4, 0xFFFFFFFF); put32(blk, 8, 0);
    put32(blk, 12, uint32_t(off)); put32(blk, 16, 0x14);
    std::memcpy(dst, blk.data(), 2048);
    for (auto& k : e.kids) emit(k, img);
}

int main(int argc, char** argv) {
    if (argc < 2) { std::fprintf(stderr, "usage: make_test_disc out.iso [LaunchMe]\n"); return 1; }
    std::vector<uint8_t> launch = make_aif();
    if (argc > 2) {
        FILE* f = std::fopen(argv[2], "rb");
        if (!f) { std::fprintf(stderr, "cannot read %s\n", argv[2]); return 1; }
        launch.clear(); int ch;
        while ((ch = std::fgetc(f)) != EOF) launch.push_back(uint8_t(ch));
        std::fclose(f);
    }
    const std::string readme = "synthetic 3DO to PC test disc\n";
    std::vector<uint8_t> test(5000);
    for (uint32_t i = 0; i < test.size(); ++i) test[i] = uint8_t((i * 7u + (i >> 8)) & 0xFF);

    Ent root{"", true, {}, {}};
    root.kids.push_back({"LaunchMe", false, launch, {}});
    Ent data{"Data", true, {}, {}};
    data.kids.push_back({"Test.bin", false, test, {}});
    root.kids.push_back(data);
    Ent docs{"Docs", true, {}, {}};
    docs.kids.push_back({"readme.txt", false, std::vector<uint8_t>(readme.begin(), readme.end()), {}});
    root.kids.push_back(docs);

    uint32_t next = 1;
    layout(root, next);
    std::vector<uint8_t> img(size_t(next) * 2048, 0);

    img[0] = 0x01; std::memset(&img[1], 0x5A, 5); img[6] = 0x01;
    std::strcpy(reinterpret_cast<char*>(&img[0x08]), "3DO to PC test");
    std::strcpy(reinterpret_cast<char*>(&img[0x28]), "TESTDISC");
    put32(img, 0x48, 0x12345678); put32(img, 0x4C, 2048); put32(img, 0x50, next);
    put32(img, 0x54, 1); put32(img, 0x58, 1); put32(img, 0x5C, 2048);
    put32(img, 0x60, 0); put32(img, 0x64, root.block);
    emit(root, img);

    FILE* f = std::fopen(argv[1], "wb");
    if (!f) return 1;
    std::fwrite(img.data(), 1, img.size(), f);
    std::fclose(f);
    return 0;
}
