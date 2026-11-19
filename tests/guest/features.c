/* features.c - exercises the CEL engine, threads/semaphores/timers and the
 * event broker from real compiled C. Exit 0x5A = all passed, else a bitmask. */
#include "pf.h"

static u32 fails;
#define CHECK(bit, cond) do { if (!(cond)) fails |= (1u << (bit)); } while (0)

void* memset(void* d, int v, unsigned n) { u8* p = d; while (n--) *p++ = (u8)v; return d; }
void* memcpy(void* d, const void* s, unsigned n) { u8* p = d; const u8* q = s; while (n--) *p++ = *q++; return d; }

static Item open_named(s32 type, const char* name) {
    TagArg tags[2] = {{1, (u32)name}, {0, 0}};
    return FindItem(type, tags);
}
static Item create_named(s32 type, const char* name) {
    TagArg tags[2] = {{1, (u32)name}, {0, 0}};
    return CreateSizedItem(type, tags, 0);
}

/* ------------------------------------------------------------ CEL engine */
static u8* fb;
static u16 px(int x, int y) { u8* p = fb + ((y >> 1) * 320 + x) * 4 + (y & 1) * 2; return (u16)((p[0] << 8) | p[1]); }
static void setpx(int x, int y, u16 v) { u8* p = fb + ((y >> 1) * 320 + x) * 4 + (y & 1) * 2; p[0] = (u8)(v >> 8); p[1] = (u8)v; }

#define F_BASE 0x3F660000u          /* abs pointers, load size/prs/ppmp, preamble in CCB, position (YOXY), both facings */
#define F_LDPLUT 0x00800000u
#define F_LAST 0x40000000u
#define F_PACKED 0x00000200u
#define PIXC_NORMAL 0x1F001F00u
#define PIXC_AVERAGE 0x1F811F81u

static u16 src16[8] = {0x7C00, 0x03E0, 0x001F, 0x7FFF, 0x0421, 0x0842, 0x0C63, 0x1084};
static u8 src4[16] = {0x12, 0x30, 0, 0, 0, 0, 0, 0,  0x44, 0x44, 0, 0, 0, 0, 0, 0};
static u16 plut[32] = {0, 0x7C00, 0x03E0, 0x001F, 0x4210};
static u8 srcpk[12] = {0x00, 0x01, 0x42, 0x01, 0x02, 0x03, 0x81, 0xC3, 0x04, 0x00, 0, 0};
static u16 src1[4] = {0x7C00, 0, 0, 0};
static CCB ccb[6];

static void setup(CCB* c, u32 flags, void* src, s32 x, s32 y, s32 hdx, s32 vdy, u32 pixc, u32 pre0, u32 pre1) {
    memset(c, 0, sizeof *c);
    c->flags = flags; c->source = src; c->plut = plut;
    c->x = x << 16; c->y = y << 16; c->hdx = hdx; c->vdy = vdy;
    c->pixc = pixc; c->pre0 = pre0; c->pre1 = pre1;
}

static void test_cels(void) {
    Item g = open_named(NODE_ID(1, 4), "Graphics");
    void** gb = (void**)LookupItem(g);
    TagArg tags[2] = {{2 /*CSG_TAG_SCREENCOUNT*/, 2}, {0, 0}};
    Item screens[2] = {0, 0};
    Item group = ((Item (*)(Item*, TagArg*))gb[-12])(screens, tags);
    CHECK(0, group > 0 && screens[0] > 0 && screens[1] > 0);
    u8* scr = LookupItem(screens[0]);
    u8* bm = *(u8**)(scr + 120);
    Item bitmap = *(Item*)(bm + 24);
    fb = *(u8**)(bm + 36);
    CHECK(0, fb != 0 && *(s32*)(bm + 40) == 320);
    for (int y = 0; y < 240; ++y) for (int x = 0; x < 320; ++x) setpx(x, y, 0x2108);

    /* 0: 16bpp uncoded 4x2 at (10,20)          1: 4bpp coded via PLUT, 2x scale at (40,40) */
    setup(&ccb[0], F_BASE, src16, 10, 20, 1 << 20, 1 << 16, PIXC_NORMAL, (1 << 6) | 0x10 | 6, 3);
    setup(&ccb[1], F_BASE | F_LDPLUT, src4, 40, 40, 2 << 20, 2 << 16, PIXC_NORMAL, (1 << 6) | 3, 3);
    /* 2: packed 8bpp coded: 3 literal, 2 transparent, 4 repeated   3: average blend 1x1 */
    setup(&ccb[2], F_BASE | F_LDPLUT | F_PACKED, srcpk, 100, 100, 1 << 20, 1 << 16, PIXC_NORMAL, 5, 0);
    setup(&ccb[3], F_BASE, src1, 200, 50, 1 << 20, 1 << 16, PIXC_AVERAGE, 0x10 | 6, 0);
    /* 4: mirrored (negative HDX) from x=250          5: clipped at the right edge */
    setup(&ccb[4], F_BASE, src16, 250, 60, -(1 << 20), 1 << 16, PIXC_NORMAL, (1 << 6) | 0x10 | 6, 3);
    setup(&ccb[5], F_BASE | F_LAST, src16, 318, 80, 1 << 20, 1 << 16, PIXC_NORMAL, (1 << 6) | 0x10 | 6, 3);
    for (int i = 0; i < 5; ++i) ccb[i].next = &ccb[i + 1];

    CHECK(1, DrawCels(bitmap, &ccb[0]) == 0);
    CHECK(2, px(10, 20) == 0x7C00 && px(11, 20) == 0x03E0 && px(13, 20) == 0x7FFF && px(10, 21) == 0x0421 &&
             px(14, 20) == 0x2108 && px(10, 22) == 0x2108);
    CHECK(3, px(40, 40) == 0x7C00 && px(41, 41) == 0x7C00 && px(42, 40) == 0x03E0 && px(45, 41) == 0x001F &&
             px(46, 40) == 0x2108 /* index 0: transparent */ && px(40, 42) == 0x4210 && px(47, 43) == 0x4210);
    CHECK(4, px(100, 100) == 0x7C00 && px(102, 100) == 0x001F && px(103, 100) == 0x2108 && px(104, 100) == 0x2108 &&
             px(105, 100) == 0x4210 && px(108, 100) == 0x4210 && px(109, 100) == 0x2108);
    CHECK(5, px(200, 50) == 0x4C84);    /* (31+8)/2, (0+8)/2, (0+8)/2 */
    CHECK(6, px(249, 60) == 0x7C00 && px(246, 60) == 0x7FFF && px(250, 60) == 0x2108);
    CHECK(7, px(318, 80) == 0x7C00 && px(319, 80) == 0x03E0);
}

/* ------------------------------------------------------ threads + timer */
static Item main_task, sem, timer_ior;
static s32 sig_a, sig_b;
static volatile s32 shared;
static u8 stack[2048] __attribute__((aligned(8)));

static void worker(void) {
    LockSemaphore(sem, 1);            /* blocks until main releases it */
    shared += 1;
    SendSignal(main_task, sig_a);
    UnlockSemaphore(sem);
    IOInfo io;
    memset(&io, 0, sizeof io);
    io.cmd = 3; io.unit = 0; io.offset = 2;   /* TIMERCMD_DELAY, VBL unit, 2 fields */
    DoIO(timer_ior, &io);             /* sleeps this thread until the next frame */
    shared += 10;
    SendSignal(main_task, sig_b);
}                                     /* returning ends the thread */

static void test_threads(void) {
    main_task = CURRENT_TASK_ITEM;
    sig_a = AllocSignal(0); sig_b = AllocSignal(0);
    sem = create_named(NODE_ID(1, 7), "test sem");
    Item timer = open_named(NODE_ID(1, 15), "timer");
    TagArg itags[2] = {{11, (u32)timer}, {0, 0}};
    timer_ior = CreateSizedItem(NODE_ID(1, 14), itags, 0);
    CHECK(8, main_task > 0 && sig_a > 0 && sig_b > 0 && sem > 0 && timer_ior > 0);

    CHECK(9, LockSemaphore(sem, 1) == 1);
    TagArg t[6] = {{1, (u32)"worker"}, {2, 150}, {10, (u32)worker}, {12, sizeof stack},
                   {15, (u32)(stack + sizeof stack)}, {0, 0}};
    Item th = CreateSizedItem(NODE_ID(1, 5), t, 0);
    CHECK(9, th > 0);
    for (volatile int i = 0; i < 20000; ++i) { }   /* worker (higher priority) runs and blocks on sem */
    CHECK(10, shared == 0);
    UnlockSemaphore(sem);              /* worker resumes */
    CHECK(11, WaitSignal(sig_a) == sig_a && shared == 1);
    CHECK(12, WaitSignal(sig_b) == sig_b && shared == 11);
}

/* ---------------------------------------------------------- event broker */
static void test_input(void) {
    Item broker = open_named(NODE_ID(1, 10), "eventbroker");
    Item port = create_named(NODE_ID(1, 10), "pad port");
    TagArg mt[2] = {{10, (u32)port}, {0, 0}};
    Item msg = CreateSizedItem(NODE_ID(1, 9), mt, 0);
    CHECK(13, broker > 0 && port > 0 && msg > 0);
    static u32 cfg[27];
    memset(cfg, 0, sizeof cfg);
    cfg[0] = 1;                        /* EB_Configure */
    cfg[1] = 1;                        /* focus listener */
    cfg[2] = 0xE0000000;               /* trigger: pressed, released, update */
    CHECK(13, SendMsg(broker, msg, cfg, sizeof cfg) == 0);
    CHECK(13, WaitPort(port, msg) == msg);

    int pressed = 0, released = 0;
    for (int n = 0; n < 500 && !released; ++n) {
        Item m = WaitPort(port, 0);    /* blocks across frames until an event arrives */
        if (m <= 0) { fails |= 1u << 14; break; }
        u32* d = MSG_DATAPTR(LookupItem(m));
        if (d[0] == 3) {               /* EB_EventRecord */
            u8* f = (u8*)(d + 1);
            while (*(u32*)f) {
                u32 ev = f[16], buttons = *(u32*)(f + 28);
                if (ev == 1 && (buttons & 0x08000000)) pressed = 1;          /* ControlA went down */
                if (ev == 2 && pressed && (buttons & 0x08000000)) released = 1; /* frame holds the button that came up */
                f += *(u32*)f;
            }
        }
        ReplyMsg(m, 0, 0, 0);
    }
    CHECK(15, pressed && released);
}

int main(void) {
    test_cels();
    test_threads();
    test_input();
    return fails ? (int)fails : 0x5A;
}
