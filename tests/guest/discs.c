/* discs.c - a two-disc game: reading a file only disc 2 has puts disc 2 in
 * the drive, so files both discs have then come from disc 2.
 * Exit 0x5A = all passed, else a bitmask. */
#include "pf.h"

static u32 fails;
#define CHECK(bit, cond) do { if (!(cond)) fails |= (1u << (bit)); } while (0)

void* memset(void* d, int v, unsigned n) { u8* p = d; while (n--) *p++ = (u8)v; return d; }
void* memcpy(void* d, const void* s, unsigned n) { u8* p = d; const u8* q = s; while (n--) *p++ = *q++; return d; }

static u8 buf[2048];

/* First byte of a file, or -1 if it cannot be opened. */
static int first_byte(const char* path) {
    Item f = OpenDiskFile(path);
    if (f < 0) return -1;
    TagArg tags[2] = {{11 /*device*/, (u32)f}, {0, 0}};
    Item ior = CreateSizedItem(NODE_ID(1, 14), tags, 0);
    IOInfo io; memset(&io, 0, sizeof io);
    io.cmd = CMD_READ; io.recv_buf = buf; io.recv_len = sizeof buf;
    buf[0] = 0;
    DoIO(ior, &io);
    DeleteItem(ior);
    CloseDiskFile(f);
    return buf[0];
}

int main(void) {
    CHECK(0, first_byte("shared.txt") == '1');          /* disc 1 is in the drive */
    CHECK(1, first_byte("only2.txt") == 'X');           /* only on disc 2: disc 2 goes in */
    CHECK(2, first_byte("/cd-rom/shared.txt") == '2');  /* and stays in */
    CHECK(3, first_byte("missing.txt") < 0);            /* on neither disc */
    return fails ? (int)fails : 0x5A;
}
