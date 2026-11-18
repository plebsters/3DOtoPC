/* api.c - OS calls filled in from the folio tables: MapCel, CheckIO,
 * QueryGraphics, graphics SWIs, memory lists, wcopy and the International
 * folio (locale, strings, numbers), text drawing, UniversalInsertNode,
 * SetFunction, the Compression and JString folios. Exit 0x5A = all passed, else a bitmask. */
#include "pf.h"

static u32 fails;
#define CHECK(bit, cond) do { if (!(cond)) fails |= (1u << (bit)); } while (0)

void* memset(void* d, int v, unsigned n) { u8* p = d; while (n--) *p++ = (u8)v; return d; }
void* memcpy(void* d, const void* s, unsigned n) { u8* p = d; const u8* q = s; while (n--) *p++ = *q++; return d; }

SWI_FN(s32, SetClipWidthSWI, 0x20013, (Item bitmap, s32 w))
SWI_FN(s32, wcopy,           0x1001B, (s32* src, s32* dst, s32 bytes))
SWI_FN(s32, Forbid,          0x1000B, (void))
SWI_FN(s32, Permit,          0x1000C, (void))
SWI_FN(void*, SetFunctionSWI, 0x10017, (Item folio, s32 num, s32 type, void* fn))

typedef struct Point { s32 x, y; } Point;
typedef u16 unichar;

static Item open_named(s32 type, const char* name) {
    TagArg tags[2] = {{1, (u32)name}, {0, 0}};
    return FindItem(type, tags);
}
static void** folio(const char* name) { return (void**)LookupItem(open_named(NODE_ID(1, 4), name)); }

static int ustreq(const unichar* a, const char* b) {
    while (*a && *b) if (*a++ != (unichar)*b++) return 0;
    return *a == 0 && *b == 0;
}

/* Sum of free pages over KernelBase->kb_MemHdrList (as availMem() does). */
static u32 free_pages(void) {
    PList* l = *(PList**)((char*)KernelBase + 120);
    u32 total = 0;
    for (PNode* n = l->head; n != (PNode*)&l->null; n = n->next) {
        u8* m = (u8*)n;
        u32* bits = *(u32**)(m + 40);
        u32 words = m[52], page = *(u32*)(m + 24);
        for (u32 i = 0; i < words; ++i)
            for (u32 v = bits[i]; v; v >>= 1) if (v & 1) total += page;
    }
    return total;
}

typedef struct Buf { u32 n; u32 w[256]; } Buf;
static void put_word(void* ud, u32 w) { Buf* b = ud; if (b->n < 256) b->w[b->n++] = w; }
static int by_pri(PNode* n, PNode* m) { return ((u8*)n)[10] > ((u8*)m)[10]; }
static s32 my_fn(void) { return 77; }
static int bytes_eq(const char* a, const char* b, int n) { while (n--) if (*a++ != *b++) return 0; return 1; }
SWI_FN(s32, SendIOSWI,  0x10018, (Item ior, IOInfo* info))
SWI_FN(s32, AbortIOSWI, 0x10019, (Item ior))
SWI_FN(s32, WaitIOSWI,  0x10029, (Item ior))
static u8 idle_stack[1024] __attribute__((aligned(8)));
static void idler(void) { for (;;) WaitSignal(0x40000000); }   /* a second thread, so timers run asynchronously */

int main(void) {
    void** gb = folio("Graphics");
    void** ib = folio("International");
    CHECK(0, gb != 0 && ib != 0);

    /* 1: MapCel - a 10x20 cel onto a 2x rectangle, then onto a trapezoid */
    CCB c; memset(&c, 0, sizeof c);
    c.width = 10; c.height = 20;
    Point r[4] = {{5, 6}, {25, 6}, {25, 46}, {5, 46}};
    ((void (*)(CCB*, Point*))gb[-1])(&c, r);
    CHECK(1, c.x == (5 << 16) && c.y == (6 << 16) && c.hdx == (2 << 20) && c.hdy == 0 &&
             c.vdx == 0 && c.vdy == (2 << 16) && c.hddx == 0 && c.hddy == 0);
    Point t[4] = {{0, 0}, {10, 0}, {14, 20}, {-4, 20}};
    ((void (*)(CCB*, Point*))gb[-1])(&c, t);
    CHECK(1, c.hdx == (1 << 20) && c.vdx == -13107 && c.vdy == (1 << 16) && c.hddx == 41943);

    /* 2: CheckIO - a finished timer request reads as done; a bad item is an error */
    Item timer = open_named(NODE_ID(1, 15), "timer");
    TagArg itags[2] = {{11, (u32)timer}, {0, 0}};
    Item ior = CreateSizedItem(NODE_ID(1, 14), itags, 0);
    u32 tv[2];
    IOInfo io; memset(&io, 0, sizeof io);
    io.cmd = 1 /* CMD_READ: current time */; io.unit = 1; io.recv_buf = tv; io.recv_len = 8;
    DoIO(ior, &io);
    s32 (*CheckIO)(Item) = (s32 (*)(Item))KV(-41);
    CHECK(2, ior > 0 && CheckIO(ior) == 1 && CheckIO(12345) < 0);

    /* 3: QueryGraphics / QueryGraphicsList */
    s32 freq = 0, w = 0, h = 0;
    s32 e = ((s32 (*)(s32, void*))gb[-48])(1, &freq);
    TagArg q[3] = {{4, (u32)&w}, {5, (u32)&h}, {0, 0}};
    s32 e2 = ((s32 (*)(TagArg*))gb[-49])(q);
    CHECK(3, e == 0 && freq == 60 && e2 == 0 && w == 320 && h == 240);

    /* 4: graphics SWIs reach the same code as the vectors (SetClipWidth) */
    TagArg st[2] = {{2, 1}, {0, 0}};
    Item screens[1] = {0};
    ((Item (*)(Item*, TagArg*))gb[-12])(screens, st);
    u8* scr = LookupItem(screens[0]);
    u8* bm = *(u8**)(scr + 120);
    Item bitmap = *(Item*)(bm + 24);
    SetClipWidthSWI(bitmap, 100);
    CHECK(4, *(s32*)(bm + 56) == 100);

    /* 5: memory lists, wcopy, Forbid/Permit */
    void* ml = ((void* (*)(void*, const char*))KV(-28))(0, "pool");
    s32* p = ((void* (*)(void*, s32, u32))KV(-26))(ml, 64, 0);
    s32 src[4] = {1, 2, 3, 4};
    if (p) wcopy(src, p, 16);
    CHECK(5, ml && p && p[0] == 1 && p[3] == 4 && Forbid() == 0 && Permit() == 0);
    if (p) ((void (*)(void*, void*, s32))KV(-27))(ml, p, 64);
    ((void (*)(void*))KV(-29))(ml);

    /* 6: the locale: English, United States */
    Item li = FindAndOpenItem(NODE_ID(9, 1), 0);
    u8* loc = li > 0 ? LookupItem(li) : 0;
    CHECK(6, loc && *(u32*)(loc + 36) == (('e' << 24) | ('n' << 16)) && *(u32*)(loc + 44) == 840);

    /* 7: intlCompareStrings */
    static const unichar abc[] = {'a', 'b', 'c', 0}, ABD[] = {'A', 'B', 'D', 0};
    static const unichar Hello[] = {'H', 'e', 'l', 'l', 'o', 0}, hello[] = {'h', 'e', 'l', 'l', 'o', 0};
    s32 (*cmp)(Item, const unichar*, const unichar*) = (s32 (*)(Item, const unichar*, const unichar*))ib[-4];
    CHECK(7, cmp(li, abc, ABD) < 0 && cmp(li, ABD, abc) > 0 && cmp(li, hello, hello) == 0 && cmp(li, Hello, hello) != 0);

    /* 8: intlConvertString (upper case; five arguments, the last on the stack) */
    unichar out[16];
    s32 n = ((s32 (*)(Item, const unichar*, unichar*, u32, u32))ib[-5])(li, Hello, out, sizeof out, 1);
    CHECK(8, n == 5 && ustreq(out, "HELLO"));

    /* 9: intlGetCharAttrs */
    u32 (*attrs)(Item, unichar) = (u32 (*)(Item, unichar))ib[-6];
    CHECK(9, (attrs(li, 'A') & 1) && (attrs(li, 'a') & 2) && (attrs(li, '5') & 8) && (attrs(li, ' ') & 64) &&
             (attrs(li, 0x3042) & 1024));

    /* 10: intlFormatNumber with the locale's number format */
    unichar num[32];
    s32 m = ((s32 (*)(Item, void*, u32, u32, s32, s32, unichar*, u32))ib[-1])(li, 0, 1234567, 0, 0, 0, num, sizeof num);
    CHECK(10, m == 9 && ustreq(num, "1,234,567"));

    /* 11: free memory as the C library's availMem() counts it - free-page bits in the bank headers */
    u32 before = free_pages(), after;
    void* big = AllocMem(256 * 1024, 0);
    after = free_pages();
    CHECK(11, big && before > 512 * 1024 && before < 3 * 1024 * 1024 && before - after >= 254 * 1024 &&
              before - after <= 262 * 1024);
    if (big) FreeMem(big, 256 * 1024);
    CHECK(11, free_pages() == before);

    /* 12: pens, MoveTo, DrawText8 (built-in 8x8 font), ReadPixel */
    u32 gc[10]; memset(gc, 0, sizeof gc);
    ((void (*)(u32*, u32))gb[-24])(gc, 0x7C00);
    ((void (*)(u32*, s32, s32))gb[-30])(gc, 10, 20);
    s32 dt = ((s32 (*)(u32*, Item, const char*))gb[-37])(gc, bitmap, "AB");
    s32 lit = 0;
    for (s32 y = 20; y < 28; ++y) for (s32 x = 10; x < 26; ++x)
        if (((u32 (*)(Item, u32*, s32, s32))gb[-2])(bitmap, gc, x, y) == 0x7C00) ++lit;
    CHECK(12, dt == 0 && gc[5] == 0x7C00 && gc[7] == 26 && gc[8] == 20 && lit > 10 && lit < 100);

    /* 13: DrawTo draws a line and moves the pen; GetCurrentFont */
    ((void (*)(u32*, s32, s32))gb[-30])(gc, 0, 100);
    ((s32 (*)(Item, u32*, s32, s32))gb[-33])(bitmap, gc, 9, 100);
    u8* font = ((u8* (*)(void))gb[-36])();
    CHECK(13, ((u32 (*)(Item, u32*, s32, s32))gb[-2])(bitmap, gc, 5, 100) == 0x7C00 && gc[7] == 9 &&
              font && font[0] == 8);

    /* 14: UniversalInsertNode (calls back into this program) keeps the list sorted by priority */
    u32 lst[8]; memset(lst, 0, sizeof lst);
    PList* L = (PList*)lst;
    ((void (*)(void*))KV(-9))(lst);        /* InitList */
    u32 na[6], nb[6], nc[6];
    memset(na, 0, sizeof na); memset(nb, 0, sizeof nb); memset(nc, 0, sizeof nc);
    ((u8*)na)[10] = 10; ((u8*)nb)[10] = 30; ((u8*)nc)[10] = 20;
    void (*uin)(void*, void*, int (*)(PNode*, PNode*)) = (void (*)(void*, void*, int (*)(PNode*, PNode*)))KV(-17);
    uin(lst, na, by_pri); uin(lst, nb, by_pri); uin(lst, nc, by_pri);
    CHECK(14, L->head == (PNode*)nb && nb[0] == (u32)nc && nc[0] == (u32)na);

    /* 15: SetFunction replaces a folio vector and returns the old one */
    void* old = (void*)SetFunctionSWI(open_named(NODE_ID(1, 4), "International"), -6, 0, (void*)my_fn);
    s32 r15 = ((s32 (*)(void))ib[-6])();
    SetFunctionSWI(open_named(NODE_ID(1, 4), "International"), -6, 0, old);
    CHECK(15, old != 0 && r15 == 77 && ib[-6] == old);

    /* 16: Compression folio: compress, then decompress, gives back the data */
    void** cf = folio("compression");
    static u32 data[64], back[64];
    for (int i = 0; i < 64; ++i) data[i] = (i % 8) * 0x01010101u + (i == 40 ? 5 : 0);
    static Buf packed, unpacked;
    TagArg ct[2] = {{10, (u32)&packed}, {0, 0}};
    void* comp = 0;
    s32 c1 = cf ? ((s32 (*)(void**, void (*)(void*, u32), TagArg*))cf[-1])(&comp, put_word, ct) : -1;
    if (!c1) { ((s32 (*)(void*, void*, u32))cf[-3])(comp, data, 64); ((s32 (*)(void*))cf[-2])(comp); }
    TagArg dtg[2] = {{10, (u32)&unpacked}, {0, 0}};
    void* dec = 0; s32 d3 = -1;
    if (!c1 && ((s32 (*)(void**, void (*)(void*, u32), TagArg*))cf[-5])(&dec, put_word, dtg) == 0) {
        ((s32 (*)(void*, void*, u32))cf[-7])(dec, packed.w, packed.n);
        d3 = ((s32 (*)(void*))cf[-6])(dec);
    }
    for (u32 i = 0; i < unpacked.n && i < 64; ++i) back[i] = unpacked.w[i];
    int same = unpacked.n == 64;
    for (int i = 0; same && i < 64; ++i) same = back[i] == data[i];
    CHECK(16, c1 == 0 && packed.n > 0 && packed.n < 40 && d3 == 0 && same);

    /* 17: JString: ASCII -> full-width Shift-JIS, romaji <-> hiragana, Shift-JIS -> Unicode */
    void** jf = folio("jstring");
    typedef s32 (*JConv)(const char*, char*, u32, u8);
    char sj[32], rj[32]; unichar uj[16];
    s32 j1 = jf ? ((JConv)jf[-4])("AB", sj, sizeof sj, '?') : -1;
    int ok1 = j1 == 4 && bytes_eq(sj, "\x82\x60\x82\x61", 5);
    s32 j2 = jf ? ((JConv)jf[-5])("kyouto", sj, sizeof sj, '?') : -1;
    int ok2 = j2 == 8 && bytes_eq(sj, "\x82\xab\x82\xe5\x82\xa4\x82\xc6", 9);
    s32 j3 = jf ? ((JConv)jf[-8])(sj, rj, sizeof rj, '?') : -1;
    int ok3 = j3 == 6 && bytes_eq(rj, "kyouto", 7);
    s32 j4 = jf ? ((s32 (*)(const char*, unichar*, u32, u8))jf[-1])(sj, uj, sizeof uj, '?') : -1;
    int ok4 = j4 == 4 && uj[0] == 0x304D && uj[1] == 0x3087 && uj[4] == 0;
    s32 j5 = jf ? ((JConv)jf[-4])("ABCDEFGHIJKLMNOP", sj, 8, '?') : 0;
    CHECK(17, ok1 && ok2 && ok3 && ok4 && j5 < 0);

    /* 18: MEMTYPE_STARTPAGE blocks start on a 2 KB page and own whole pages */
    void* small1 = AllocMem(64, 0);
    u8* pg = AllocMem(5000, 0x02000000 | 0x00010000);   /* STARTPAGE | VRAM */
    void* small2 = AllocMem(64, 0);
    CHECK(18, pg && ((u32)pg & 2047) == 0 && (((u32)small2 + 64 <= (u32)pg) || ((u32)small2 >= (u32)pg + 6144)));
    if (pg) FreeMem(pg, 5000);
    if (small1) FreeMem(small1, 64);
    if (small2) FreeMem(small2, 64);

    /* 19: SendIO on a timer returns at once and completes later; WaitIO waits for it */
    TagArg it19[5] = {{1, (u32)"idler"}, {2, 50}, {10, (u32)idler}, {12, sizeof idle_stack}, {0, 0}};
    TagArg it19b[6] = {{1, (u32)"idler"}, {2, 50}, {10, (u32)idler}, {12, sizeof idle_stack},
                       {15, (u32)(idle_stack + sizeof idle_stack)}, {0, 0}};
    (void)it19;
    Item idle = CreateSizedItem(NODE_ID(1, 5), it19b, 0);
    IOInfo d; memset(&d, 0, sizeof d);
    d.cmd = 3; d.unit = 0; d.offset = 10;          /* TIMERCMD_DELAY, VBL unit, 10 fields */
    s32 s19 = SendIOSWI(ior, &d);
    s32 pending19 = CheckIO(ior);
    s32 w19 = WaitIOSWI(ior);
    CHECK(19, idle > 0 && s19 == 0 && pending19 == 0 && w19 == 0 && CheckIO(ior) == 1);

    /* 20: AbortIO ends a pending request with ER_Aborted; the next SendIO starts clean */
    s32 s20 = SendIOSWI(ior, &d);
    AbortIOSWI(ior);
    s32 w20 = WaitIOSWI(ior);
    s32 s20b = SendIOSWI(ior, &d);
    s32 w20b = WaitIOSWI(ior);
    CHECK(20, s20 == 0 && w20 == (s32)0x8101010A && s20b == 0 && w20b == 0);

    /* 21: a pass-by-value message (CREATEMSG_TAG_DATA_SIZE) gets a copy of
       the data in its own area; ReplyMsg copies the reply there too */
    TagArg pt[2] = {{1, (u32)"vport"}, {0, 0}};
    Item vport = CreateSizedItem(NODE_ID(1, 10), pt, 0);   /* MSGPORTNODE */
    TagArg mt[3] = {{10, (u32)vport}, {12, 16}, {0, 0}};
    Item vmsg = CreateSizedItem(NODE_ID(1, 9), mt, 0);     /* MESSAGENODE */
    u32 payload[2] = {0x11223344, 0x55667788};
    SendMsg(vport, vmsg, payload, 8);
    payload[0] = 0;                                  /* the copy must not follow the original */
    Item got = GetMsg(vport);
    void* mp = LookupItem(vmsg);
    u32* area = mp ? (u32*)MSG_DATAPTR(mp) : 0;
    const int sent_ok = area && area != payload && area[0] == 0x11223344 && area[1] == 0x55667788;
    u32 reply = 0x99AABBCC;
    ReplyMsg(vmsg, 0, &reply, 4);
    CHECK(21, vport > 0 && vmsg > 0 && got == vmsg && sent_ok &&
              (u32*)MSG_DATAPTR(mp) == area && area[0] == 0x99AABBCC && MSG_DATASIZE(mp) == 4);

    return fails ? (int)fails : 0x5A;
}
