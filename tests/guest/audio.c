/* audio.c - exercises the audio folio from compiled C: instruments, a mixer,
 * knobs, a sample made in memory, an AIFF parsed from memory, a cue at the
 * end of an attachment, linked attachments and item info.
 * Exit 0x5A = all passed, else a bitmask of failed checks. */
#include "pf.h"

static u32 fails;
#define CHECK(bit, cond) do { if (!(cond)) fails |= (1u << (bit)); } while (0)

void* memset(void* d, int v, unsigned n) { u8* p = d; while (n--) *p++ = (u8)v; return d; }
void* memcpy(void* d, const void* s, unsigned n) { u8* p = d; const u8* q = s; while (n--) *p++ = *q++; return d; }

SWI_FN(s32, TweakKnob,          0x40000, (Item knob, s32 value))
SWI_FN(s32, StartInstrument,    0x40001, (Item ins, TagArg* tags))
SWI_FN(s32, StopInstrument,     0x40003, (Item ins, TagArg* tags))
SWI_FN(s32, ConnectInstruments, 0x40008, (Item src, const char* sn, Item dst, const char* dn))
SWI_FN(s32, StartAttachment,    0x40012, (Item att, TagArg* tags))
SWI_FN(s32, LinkAttachments,    0x40015, (Item a, Item b))
SWI_FN(s32, MonitorAttachment,  0x40016, (Item att, Item cue, s32 index))
SWI_FN(u32, GetAudioFrameCount, 0x40026, (void))

static void** ab;   /* audio folio base */
#define LoadInstrument(n, d, p)   ((Item (*)(const char*, Item, u32))ab[-10])(n, d, p)
#define GrabKnob(i, n)            ((Item (*)(Item, const char*))ab[-4])(i, n)
#define MakeSample(nb, t)         ((Item (*)(u32, TagArg*))ab[-14])(nb, t)
#define DefineSampleHere(p, n)    ((Item (*)(void*, u32, void*, void*))ab[-41])(p, n, 0, 0)
#define AttachSample(i, s, f)     ((Item (*)(Item, Item, const char*))ab[-36])(i, s, f)
#define GetAudioItemInfo(i, t)    ((s32 (*)(Item, TagArg*))ab[-9])(i, t)
#define GetCueSignal(c)           ((u32 (*)(Item))ab[-18])(c)

static u8 aiff[] = {
    'F','O','R','M', 0,0,0,46+200, 'A','I','F','F',
    'C','O','M','M', 0,0,0,18,  0,1,  0,0,0,100,  0,16,  0x40,0x0D,0xAC,0x44,0,0,0,0,0,0,   /* 1 ch, 100 frames, 16 bit, 22050 Hz */
    'S','S','N','D', 0,0,0,8+200, 0,0,0,0, 0,0,0,0,
};
static u8 aiff_image[sizeof aiff + 200];

int main(void) {
    TagArg ft[2] = {{1, (u32)"audio"}, {0, 0}};
    ab = (void**)LookupItem(FindItem(NODE_ID(1, 4), ft));
    CHECK(0, ab != 0);

    Item ins = LoadInstrument("sampler.dsp", 0, 100);
    Item mix = LoadInstrument("mixer4x2.dsp", 0, 100);
    CHECK(1, ins > 0 && mix > 0);

    /* 4000 frames of a square wave in a sample we allocate through the folio. */
    TagArg st[3] = {{23 /*CHANNELS*/, 1}, {22 /*WIDTH*/, 2}, {0, 0}};
    Item smp = MakeSample(8000, st);
    TagArg info[3] = {{36 /*ADDRESS*/, 0}, {24 /*FRAMES*/, 0}, {0, 0}};
    GetAudioItemInfo(smp, info);
    s16* data = (s16*)info[0].arg;
    CHECK(2, smp > 0 && data != 0 && info[1].arg == 4000);
    for (int i = 0; i < 4000; ++i) data[i] = (i / 50) & 1 ? 12000 : -12000;

    /* A second sample: an AIFF image parsed in memory, linked after the first. */
    memcpy(aiff_image, aiff, sizeof aiff);
    Item smp2 = DefineSampleHere(aiff_image, sizeof aiff_image);
    TagArg info2[3] = {{24 /*FRAMES*/, 0}, {46 /*SAMPLE_RATE*/, 0}, {0, 0}};
    GetAudioItemInfo(smp2, info2);
    CHECK(3, smp2 > 0 && info2[0].arg == 100 && info2[1].arg == (22050u << 16));

    Item att = AttachSample(ins, smp, 0);
    Item att2 = AttachSample(ins, smp2, 0);
    CHECK(4, att > 0 && att2 > 0);
    LinkAttachments(att, att2);

    CHECK(5, ConnectInstruments(ins, "Output", mix, "Input0") == 0);
    Item lg = GrabKnob(mix, "LeftGain0"), rg = GrabKnob(mix, "RightGain0");
    CHECK(6, lg > 0 && rg > 0);
    TweakKnob(lg, 0x4000); TweakKnob(rg, 0x4000);

    /* Cues: one when the first sample ends, one when the linked second ends. */
    Item cue = CreateSizedItem(NODE_ID(4, 5), 0, 0);
    Item cue2 = CreateSizedItem(NODE_ID(4, 5), 0, 0);
    u32 sig = GetCueSignal(cue), sig2 = GetCueSignal(cue2);
    CHECK(7, cue > 0 && sig && sig2 && sig != sig2);
    MonitorAttachment(att, cue, -2 /*CUE_AT_END*/);
    MonitorAttachment(att2, cue2, -2);

    u32 before = GetAudioFrameCount();
    StartInstrument(mix, 0);
    TagArg rate[2] = {{11 /*RATE*/, 0x4000}, {0, 0}};   /* 1.0: one sample per output frame */
    StartInstrument(ins, rate);
    u32 got = WaitSignal(sig);
    u32 mid = GetAudioFrameCount();
    CHECK(8, (got & sig) != 0 && mid - before >= 4000 && mid - before < 4000 + 2000);
    got = WaitSignal(sig2);
    CHECK(9, (got & sig2) != 0);
    StopInstrument(ins, 0);
    return fails ? (int)fails : 0x5A;
}
