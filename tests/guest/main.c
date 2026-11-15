/* main.c - exercises the kernel and file HLE from real compiled C.
 * Exit code 0x5A = everything passed; otherwise a bitmask of failed checks. */
#include "pf.h"

/* KernelBase is defined and set by crt0.S */

static u32 fails;
#define CHECK(bit, cond) do { if (!(cond)) fails |= (1u << (bit)); } while (0)

/* The compiler may emit calls to these for struct copies/clears. */
void* memset(void* d, int v, unsigned n) { u8* p = d; while (n--) *p++ = (u8)v; return d; }
void* memcpy(void* d, const void* s, unsigned n) { u8* p = d; const u8* q = s; while (n--) *p++ = *q++; return d; }

static int streq(const char* a, const char* b) { while (*a && *a == *b) { ++a; ++b; } return *a == *b; }

/* Same generator as make_test_disc's Data/Test.bin */
static u8 pattern(u32 i) { return (u8)((i * 7u + (i >> 8)) & 0xFF); }

static Item open_folio(const char* name) {
    TagArg tags[2] = {{1 /*TAG_ITEM_NAME*/, (u32)name}, {0, 0}};
    return FindAndOpenItem(NODE_ID(1, 4), tags);
}

static Item create_ioreq(Item device) {
    TagArg tags[2] = {{11 /*device*/, (u32)device}, {0, 0}};
    return CreateSizedItem(NODE_ID(1, 14), tags, 0);
}

static s32 do_cmd(Item ior, u8 cmd, s32 offset, void* buf, s32 len) {
    IOInfo io;
    memset(&io, 0, sizeof io);
    io.cmd = cmd; io.offset = offset; io.recv_buf = buf; io.recv_len = len;
    return DoIO(ior, &io);
}

static u8 big[6144];

int main(int argc, char** argv) {
    /* 0: start-up registers */
    CHECK(0, argc == 1 && argv && argv[0] && streq(argv[0], "LaunchMe") && KernelBase);

    /* 1-2: folios by name, LookupItem */
    Item g = open_folio("graphics");                 /* case-insensitive */
    CHECK(1, g > 0);
    CHECK(2, g > 0 && LookupItem(g) != 0);
    CHECK(2, open_folio("NoSuchFolio") < 0);

    /* 3: memory */
    u8* p = AllocMem(1000, MEMTYPE_FILL | 0x5A);
    CHECK(3, p && p[0] == 0x5A && p[999] == 0x5A);
    if (p) FreeMem(p, 1000);
    u8* q = AllocMem(1000, 0);
    CHECK(3, q == p);                                /* freed block is reused */

    /* 4: lists */
    PList l; PNode a, b, c, d;
    memset(&a, 0, sizeof a); memset(&b, 0, sizeof b); memset(&c, 0, sizeof c); memset(&d, 0, sizeof d);
    a.name = "alpha"; b.name = "beta"; c.name = "gamma"; d.name = "delta";
    InitList(&l, "test");
    CHECK(4, l.head == (PNode*)&l.null);             /* empty */
    AddTail(&l, &a); AddTail(&l, &b); AddHead(&l, &c);      /* c a b */
    CHECK(4, l.head == &c && c.next == &a && a.next == &b && l.tailpred == &b);
    CHECK(4, FindNamedNode(&l, "BETA") == &b);
    CHECK(4, RemHead(&l) == &c && RemTail(&l) == &b && l.head == &a && l.tailpred == &a);
    a.pri = 10; d.pri = 20;
    InsertNodeFromTail(&l, &d);                      /* higher priority goes first */
    CHECK(4, l.head == &d && d.next == &a);
    CHECK(4, RemHead(&l) == &d && RemHead(&l) == &a && RemHead(&l) == 0);

    /* 5: signals and time */
    s32 sig = AllocSignal(0);
    CHECK(5, sig >= 0x100);
    u32 t0 = SampleSystemTime();
    CHECK(5, WaitSignal(sig) == sig);
    u32 t1 = SampleSystemTime();
    CHECK(5, t1 > t0);

    /* 6-8: files */
    Item f = OpenDiskFile("data/TEST.BIN");          /* relative + case-insensitive */
    CHECK(6, f > 0);
    Item ior = create_ioreq(f);
    CHECK(6, ior > 0);
    FileStatus st;
    memset(&st, 0, sizeof st);
    CHECK(7, do_cmd(ior, CMD_STATUS, 0, &st, sizeof st) == 0);
    CHECK(7, st.byte_count == 5000 && st.block_size == 2048 && st.block_count == 3);
    CHECK(8, do_cmd(ior, CMD_READ, 0, big, sizeof big) == 0);
    u32 ok = 1, i;
    for (i = 0; i < 5000; ++i) if (big[i] != pattern(i)) { ok = 0; break; }
    for (; i < sizeof big; ++i) if (big[i] != 0) { ok = 0; break; }  /* tail of last block */
    CHECK(8, ok);
    CHECK(8, do_cmd(ior, CMD_READ, 2, big, 2048) == 0 && big[0] == pattern(4096));
    DeleteItem(ior);
    CloseDiskFile(f);

    /* 9: absolute paths, ChangeDirectory, directories */
    Item f2 = OpenDiskFile("/cd-rom/Data/Test.bin");
    CHECK(9, f2 > 0);
    CloseDiskFile(f2);
    CHECK(9, OpenDiskFile("missing.dat") < 0);
    Item dir = ChangeDirectory("/cd-rom/DATA");
    CHECK(9, dir > 0);
    Item f3 = OpenDiskFile("test.bin");
    CHECK(9, f3 > 0);
    Item dir_ior = create_ioreq(dir);
    DirEntry de;
    memset(&de, 0, sizeof de);
    CHECK(10, do_cmd(dir_ior, FILECMD_READDIR, 1, &de, sizeof de) == 0 && streq(de.name, "Test.bin") &&
              de.byte_count == 5000);
    CHECK(10, do_cmd(dir_ior, FILECMD_READDIR, 2, &de, sizeof de) < 0);    /* only one entry */
    char path[64];
    CHECK(10, do_cmd(dir_ior, FILECMD_GETPATH, 0, path, sizeof path) == 0 && streq(path, "/cd-rom/Data"));

#ifdef CRASH_PROBE
    /* What a call through a NULL pointer does in a game linked at address 0:
     * jumps into the AIF header. The runtime must stop and report it. */
    ((void (*)(void))0x00010000)();
#endif
    return fails ? (int)fails : 0x5A;
}
