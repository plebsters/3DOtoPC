#include <cstdlib>
// hle_graphics.cpp - Graphics folio replacement (screen groups, display,
// CEL entry points) plus the other folios games open at start-up.
#include "hle_os.h"
#include "tdort/gfx.h"
#include <algorithm>
#include <cmath>

namespace tdort {

using namespace abi;

int32_t Portfolio::create_screen_group(uint32_t screen_items_out, uint32_t tags) {
    uint32_t screens = 1, bitmaps = 1, height = 240, width_arr = 0, height_arr = 0, buf_arr = 0;
    each_tag(tags, [&](uint32_t t, uint32_t a) {
        switch (t) {
        case kCsgScreenCount: screens = a; break;
        case kCsgBitmapCount: bitmaps = a; break;
        case kCsgScreenHeight: case kCsgDisplayHeight: height = a; break;
        case kCsgBitmapWidthArray: width_arr = a; break;
        case kCsgBitmapHeightArray: height_arr = a; break;
        case kCsgBitmapBufArray: buf_arr = a; break;
        default: break;
        }
    });
    screens = std::clamp<uint32_t>(screens, 1, 8);
    bitmaps = std::clamp<uint32_t>(bitmaps, 1, 8);

    HleItem& g = new_item(ItemKind::ScreenGroup, kSubsysGraphics, kNodeScreenGroup, 64, "ScreenGroup");
    const int32_t group = g.num;
    for (uint32_t s = 0; s < screens; ++s) {
        HleItem& scr = new_item(ItemKind::Screen, kSubsysGraphics, kNodeScreen, screen::Size, "Screen");
        const int32_t scr_num = scr.num;
        const uint32_t scr_addr = scr.addr;
        list_init(scr_addr + screen::BitmapList, 0);
        w32(scr_addr + screen::ScreenGroupPtr, item(group)->addr);
        w32(scr_addr + screen::BitmapCount, bitmaps);
        for (uint32_t b = 0; b < bitmaps; ++b) {
            uint32_t w = width_arr ? r32(width_arr + 4 * b) : 320;
            uint32_t h = height_arr ? r32(height_arr + 4 * b) : height;
            uint32_t buf = buf_arr ? r32(buf_arr + 4 * (s * bitmaps + b)) : 0;
            if (!buf) buf = alloc(w * h * 2, kMemVram);
            HleItem& bm = new_item(ItemKind::Bitmap, kSubsysGraphics, kNodeBitmap, bitmap::Size, "Bitmap");
            bm.buffer = buf;
            w32(bm.addr + bitmap::Buffer, buf);
            w32(bm.addr + bitmap::Width, w);
            w32(bm.addr + bitmap::Height, h);
            w32(bm.addr + bitmap::ClipWidth, w);
            w32(bm.addr + bitmap::ClipHeight, h);
            list_add_tail(scr_addr + screen::BitmapList, bm.addr);
            if (b == 0) w32(scr_addr + screen::TempBitmap, bm.addr);
            item(scr_num)->children.push_back(bm.num);
        }
        item(group)->children.push_back(scr_num);
        if (screen_items_out) w32(screen_items_out + 4 * s, uint32_t(scr_num));
    }
    rt.log("CreateScreenGroup: %u screen(s) x %u bitmap(s) -> group %d", screens, bitmaps, group);
    return group;
}

// The frame buffer address of pixel (x, y) in a screen or bitmap item.
// 3DO bitmaps are LR-form: each 32-bit word holds one column of two lines,
// the even line in the high half. Returns 0 for a bad item or position.
uint32_t Portfolio::pixel_address(int32_t item_num, int32_t x, int32_t y) {
    HleItem* it = item(item_num);
    if (it && it->kind == ItemKind::Screen && !it->children.empty()) it = item(it->children[0]);
    if (!it || it->kind != ItemKind::Bitmap) return 0;
    const int32_t w = int32_t(r32(it->addr + bitmap::Width)), h = int32_t(r32(it->addr + bitmap::Height));
    if (x < 0 || y < 0 || x >= w || y >= h) return 0;
    return r32(it->addr + bitmap::Buffer) + uint32_t(((y >> 1) * w + x) * 4 + (y & 1) * 2);
}

int32_t Portfolio::display_screen(int32_t s0, int32_t /*s1: odd field, same image here*/) {
    HleItem* scr = item(s0);
    if (!scr || scr->kind != ItemKind::Screen || scr->children.empty()) return kErrBadItem;
    // A display list set with SetVDL decides which memory the screen shows.
    if (auto v = screen_vdl_buffer_.find(s0); v != screen_vdl_buffer_.end()) rt.gfx->set_display_buffer(v->second);
    else if (HleItem* bm = item(scr->children[0])) rt.gfx->set_display_buffer(r32(bm->addr + bitmap::Buffer));
    rt.request_vblank_wait();
    return 0;
}

int32_t Portfolio::draw_cels(int32_t bitmap_item, uint32_t ccb) {
    HleItem* bm = item(bitmap_item);
    if (!bm || bm->kind != ItemKind::Bitmap) return kErrBadItem;
    CelTarget t;
    t.buffer = r32(bm->addr + bitmap::Buffer);
    t.width = int32_t(r32(bm->addr + bitmap::Width));
    t.height = int32_t(r32(bm->addr + bitmap::Height));
    t.clip_x = int32_t(r32(bm->addr + bitmap::ClipX));
    t.clip_y = int32_t(r32(bm->addr + bitmap::ClipY));
    t.clip_w = int32_t(r32(bm->addr + bitmap::ClipWidth));
    t.clip_h = int32_t(r32(bm->addr + bitmap::ClipHeight));
    if (t.width <= 0 || t.width > 1280) t.width = 320;
    if (t.height <= 0 || t.height > 960) t.height = 240;
    if (t.clip_w <= 0) t.clip_w = t.width;
    if (t.clip_h <= 0) t.clip_h = t.height;
    rt.gfx->draw_cels(t, ccb);
    return 0;
}

void Portfolio::install_graphics() {
    std::map<int32_t, HleFn> v;
    v[gvCreateScreenGroup] = [this](Cpu*) { c.r[0] = uint32_t(create_screen_group(arg(0), arg(1))); };
    v[gvAddScreenGroup]    = [this](Cpu*) { c.r[0] = 0; };
    v[gvRemoveScreenGroup] = [this](Cpu*) { c.r[0] = 0; };
    v[gvDeleteScreenGroup] = [this](Cpu*) { c.r[0] = 0; };
    v[gvDisplayScreen]     = [this](Cpu*) { c.r[0] = uint32_t(display_screen(int32_t(arg(0)), int32_t(arg(1)))); };
    v[gvGetPixelAddress]   = [this](Cpu*) {                // GetPixelAddress(screen or bitmap, x, y)
        c.r[0] = pixel_address(int32_t(arg(0)), int32_t(arg(1)), int32_t(arg(2)));
    };
    v[gvReadPixel]         = [this](Cpu*) {                // ReadPixel(bitmap, gc, x, y)
        const uint32_t a = pixel_address(int32_t(arg(0)), int32_t(arg(2)), int32_t(arg(3)));
        c.r[0] = a && guest_range_ok(a, 2) ? uint32_t(rt.mem.ram[a] << 8 | rt.mem.ram[a + 1]) : 0;
    };
    v[gvWritePixel]        = [this](Cpu*) {                // WritePixel(bitmap, gc, x, y): foreground pen
        const uint32_t a = pixel_address(int32_t(arg(0)), int32_t(arg(2)), int32_t(arg(3)));
        const uint32_t pen = arg(1) ? r32(arg(1)) : 0x7FFF;   // GrafCon gc_FGPen is its first field
        if (a && guest_range_ok(a, 2)) { rt.mem.ram[a] = uint8_t(pen >> 8); rt.mem.ram[a + 1] = uint8_t(pen); rt.mem.invalidate(a, 2); }
        c.r[0] = 0;
    };
    // Display lists (VDLs). A VDL starts with a control word, then the address
    // of the frame buffer to show; games point screens at their own buffers
    // this way (Slam 'n Jam's movie player). Colours are not modelled.
    v[gvSubmitVDL]         = [this](Cpu*) {                // SubmitVDL(vdl, length, type) -> item
        HleItem& it = new_item(ItemKind::Generic, kSubsysGraphics, 6, 16, "VDL");
        vdl_buffer_[it.num] = arg(0) && guest_range_ok(arg(0), 8) ? r32(arg(0) + 4) : 0;
        c.r[0] = uint32_t(it.num);
    };
    v[gvSetVDL]            = [this](Cpu*) {                // SetVDL(screen, vdl item) -> previous VDL
        const int32_t scr = int32_t(arg(0)), vdl = int32_t(arg(1));
        int32_t old = 0;
        if (auto o = screen_vdl_.find(scr); o != screen_vdl_.end()) old = o->second;
        auto b = vdl_buffer_.find(vdl);
        if (b != vdl_buffer_.end() && b->second && guest_range_ok(b->second, 4)) {
            screen_vdl_[scr] = vdl;
            screen_vdl_buffer_[scr] = b->second;
        }
        c.r[0] = uint32_t(old);
    };
    v[gvModifyVDL]         = [this](Cpu*) { c.r[0] = 0; };
    // FillRect(bitmap, GrafCon*, Rect*): the rectangle, edges included, in the
    // GrafCon's foreground pen (its first field), clipped to the clip window.
    // Escape from Monster Manor paints its floor and ceiling this way.
    v[-34]                 = [this](Cpu*) {
        HleItem* bm = item(int32_t(arg(0)));
        if (!bm || bm->kind != ItemKind::Bitmap || !arg(2)) { c.r[0] = uint32_t(kErrBadItem); return; }
        const uint32_t buf = r32(bm->addr + bitmap::Buffer);
        const int32_t w = int32_t(r32(bm->addr + bitmap::Width)), h = int32_t(r32(bm->addr + bitmap::Height));
        int32_t cx = int32_t(r32(bm->addr + bitmap::ClipX)), cy = int32_t(r32(bm->addr + bitmap::ClipY));
        int32_t cw = int32_t(r32(bm->addr + bitmap::ClipWidth)), ch = int32_t(r32(bm->addr + bitmap::ClipHeight));
        if (cw <= 0) cw = w; if (ch <= 0) ch = h;
        const uint16_t pen = uint16_t(arg(1) ? r32(arg(1)) : 0);
        const int32_t x0 = std::max(int32_t(r32(arg(2))) + cx, cx), y0 = std::max(int32_t(r32(arg(2) + 4)) + cy, cy);
        const int32_t x1 = std::min(int32_t(r32(arg(2) + 8)) + cx, std::min(cx + cw, w) - 1);
        const int32_t y1 = std::min(int32_t(r32(arg(2) + 12)) + cy, std::min(cy + ch, h) - 1);
        for (int32_t y = y0; y <= y1; ++y)
            for (int32_t x = x0; x <= x1; ++x) {
                const uint32_t a = buf + uint32_t(((y >> 1) * w + x) * 4 + ((y & 1) ? 2 : 0));   // LR form
                if (a < kRamEnd - 1) { rt.mem.ram[a] = uint8_t(pen >> 8); rt.mem.ram[a + 1] = uint8_t(pen); }
            }
        if (x1 >= x0 && y1 >= y0) rt.mem.invalidate(buf, uint32_t(w * h * 2));
        c.r[0] = 0;
    };
    // The clip window of a bitmap. Its origin is also where cel coordinates
    // start: a 3D view sets it to the screen's centre and draws around (0,0)
    // (Escape from Monster Manor's walls).
    v[gvSetClipOrigin]     = [this](Cpu*) {                // SetClipOrigin(bitmap, x, y)
        HleItem* bm = item(int32_t(arg(0)));
        if (!bm || bm->kind != ItemKind::Bitmap) { c.r[0] = uint32_t(kErrBadItem); return; }
        const int32_t w = int32_t(r32(bm->addr + bitmap::Width)), h = int32_t(r32(bm->addr + bitmap::Height));
        const int32_t x = int32_t(arg(1)), y = int32_t(arg(2));
        if (x < 0 || y < 0 || x >= w || y >= h) { c.r[0] = uint32_t(kErrBadPtr); return; }
        w32(bm->addr + bitmap::ClipX, uint32_t(x)); w32(bm->addr + bitmap::ClipY, uint32_t(y));
        // keep the window inside the bitmap
        if (int32_t(r32(bm->addr + bitmap::ClipWidth)) > w - x) w32(bm->addr + bitmap::ClipWidth, uint32_t(w - x));
        if (int32_t(r32(bm->addr + bitmap::ClipHeight)) > h - y) w32(bm->addr + bitmap::ClipHeight, uint32_t(h - y));
        c.r[0] = 0;
    };
    v[gvSetClipWidth]      = [this](Cpu*) {                // SetClipWidth(bitmap, width)
        HleItem* bm = item(int32_t(arg(0)));
        if (!bm || bm->kind != ItemKind::Bitmap) { c.r[0] = uint32_t(kErrBadItem); return; }
        const int32_t w = int32_t(r32(bm->addr + bitmap::Width)) - int32_t(r32(bm->addr + bitmap::ClipX));
        w32(bm->addr + bitmap::ClipWidth, uint32_t(std::clamp<int32_t>(int32_t(arg(1)), 1, std::max(1, w))));
        c.r[0] = 0;
    };
    v[gvSetClipHeight]     = [this](Cpu*) {                // SetClipHeight(bitmap, height)
        HleItem* bm = item(int32_t(arg(0)));
        if (!bm || bm->kind != ItemKind::Bitmap) { c.r[0] = uint32_t(kErrBadItem); return; }
        const int32_t h = int32_t(r32(bm->addr + bitmap::Height)) - int32_t(r32(bm->addr + bitmap::ClipY));
        w32(bm->addr + bitmap::ClipHeight, uint32_t(std::clamp<int32_t>(int32_t(arg(1)), 1, std::max(1, h))));
        c.r[0] = 0;
    };
    v[gvDrawCels]          = [this](Cpu*) { c.r[0] = uint32_t(draw_cels(int32_t(arg(0)), arg(1))); };
    v[gvDrawScreenCels]    = [this](Cpu*) {
        HleItem* scr = item(int32_t(arg(0)));
        c.r[0] = (scr && !scr->children.empty()) ? uint32_t(draw_cels(scr->children[0], arg(1))) : uint32_t(kErrBadItem);
    };
    for (int32_t ok : {gvSetCEControl, gvSetCEWatchdog, gvResetScreenColors, gvSetScreenColor, gvSetScreenColors,
                       gvEnableVAvg, gvDisableVAvg, gvEnableHAvg, gvDisableHAvg, gvSetVBLAttrs, gvGetVBLAttrs,
                       gvSetFGPen, gvSetBGPen, gvMoveTo})
        v[ok] = [this, ok](Cpu*) {
            if (rt.cfg.trace) rt.log("Graphics[%d] accepted", ok);
            c.r[0] = 0;
        };
    graphics_base_ = make_folio("Graphics", kSubsysGraphics, 128, v);
    // Fields games read directly from the folio structure.
    const uint32_t g = graphics_base_;
    w32(g + graffolio::VRAMPageSize, 2048);
    w32(g + graffolio::DefaultDisplayWidth, 320);
    w32(g + graffolio::DefaultDisplayHeight, 240);
    w32(g + graffolio::VBLTime, 16667);
    w32(g + graffolio::VBLFreq, 60);
    w32(g + graffolio::ZeroPage, alloc(2048, kMemVram));   // all-zero VRAM page (SPORT clears)
    w32(g + graffolio::VIRSPage, alloc(2048, kMemVram));

    auto G = [this](uint32_t n, const char* name, HleFn fn) { rt.register_swi(kGraphicsSwi + n, name, std::move(fn)); };
    G(kDrawCelsSwi, "DrawCels", [this](Cpu*) { c.r[0] = uint32_t(draw_cels(int32_t(arg(0)), arg(1))); });
    G(kDrawScreenCelsSwi, "DrawScreenCels", [this](Cpu*) {
        HleItem* scr = item(int32_t(arg(0)));
        c.r[0] = (scr && !scr->children.empty()) ? uint32_t(draw_cels(scr->children[0], arg(1))) : uint32_t(kErrBadItem);
    });
}

uint32_t Portfolio::audio_ticks() const { return frame_no_ * 4; }   // 240 Hz at 60 frames/s

uint32_t Portfolio::cue_signal(int32_t cue) {
    auto it = cue_signals_.find(cue);
    if (it != cue_signals_.end()) return it->second;
    // Taken from the top of the free signals: games reserve specific low bits
    // for themselves with AllocSignal(mask) after creating their cues (Real
    // Pinball's music player asks for 0x700 after making four cues).
    uint32_t sig = 0;
    for (uint32_t b = 30; b >= kSigFirstUser; --b)
        if (!(cur().allocated & (1u << b))) { sig = 1u << b; break; }
    cur().allocated |= sig;
    w32(cur().addr + task::AllocatedSigs, cur().allocated);
    cue_signals_[cue] = sig;
    return sig;
}

void Portfolio::audio_sleep_until(uint32_t ticks) {
    c.r[0] = 0;
    const uint32_t now = audio_ticks();
    if (std::getenv("TDORT_SLEEP_TRACE")) rt.log("sleep-until %u now %u (%+d)", ticks, now, int32_t(ticks - now));
    if (int32_t(ticks - now) <= 0) return;
    const uint32_t frames = (ticks - now + 3) / 4;
    if (multitasking()) sleep_until(frame_no_ + frames);
    else rt.request_vblank_wait();
}

void Portfolio::install_other_folios() {
    // Opened by name at start-up; their calls are logged until implemented.
    // Audio folio: no sound output yet, but its clock must run. Games pace
    // music and effects with it (Gex waits on SleepUntilTime at start-up).
    // ~240 ticks per second, derived from the frame counter so it advances
    // even when every task is waiting.
    std::map<int32_t, HleFn> av;
    av[-42] = [this](Cpu*) { c.r[0] = audio_ticks(); };                          // GetAudioTime()
    av[-15] = [this](Cpu*) { c.r[0] = 240u << 16; };                             // GetAudioRate() (16.16)
    av[-16] = [this](Cpu*) { c.r[0] = 1; };                                      // GetAudioDuration()
    av[-19] = [this](Cpu*) { c.r[0] = 0; };                                      // OwnAudioClock()
    av[-20] = [this](Cpu*) { c.r[0] = 0; };                                      // DisownAudioClock()
    av[-18] = [this](Cpu*) { c.r[0] = cue_signal(int32_t(arg(0))); };            // GetCueSignal(cue)
    av[-17] = [this](Cpu*) { audio_sleep_until(arg(1)); };                       // SleepUntilTime(cue, time)
    av[-5]  = [this](Cpu*) { audio_sleep_until(audio_ticks() + arg(0)); };       // SleepAudioTicks(ticks)
    install_audio(av);
    make_folio("audio", kSubsysAudio, 128, av);
    // Operamath vectors: 16.16 fixed-point arithmetic (Real Pinball's table
    // physics). Division by zero saturates, as the hardware routine does.
    std::map<int32_t, HleFn> mv;
    auto udiv = [](uint32_t a, uint32_t b, uint32_t* rem) -> uint32_t {
        if (!b) { if (rem) *rem = 0; return 0xFFFFFFFFu; }
        const uint64_t n = uint64_t(a) << 16;
        if (rem) *rem = uint32_t(n % b);
        const uint64_t q = n / b;
        return q > 0xFFFFFFFFull ? 0xFFFFFFFFu : uint32_t(q);
    };
    auto sdiv = [](int32_t a, int32_t b, int32_t* rem) -> int32_t {
        if (!b) { if (rem) *rem = 0; return a < 0 ? INT32_MIN : INT32_MAX; }
        const int64_t n = int64_t(a) * 65536;
        if (rem) *rem = int32_t(n % b);
        const int64_t q = n / b;
        return int32_t(std::clamp<int64_t>(q, INT32_MIN, INT32_MAX));
    };
    mv[-1] = [this](Cpu*) { c.r[0] = uint32_t((uint64_t(arg(0)) * arg(1)) >> 16); };                        // MulUF16
    mv[-2] = [this](Cpu*) { c.r[0] = uint32_t((int64_t(int32_t(arg(0))) * int32_t(arg(1))) >> 16); };       // MulSF16
    mv[-3] = [this, udiv](Cpu*) { c.r[0] = udiv(arg(0), arg(1), nullptr); };                                 // DivUF16(d1, d2)
    mv[-4] = [this, udiv](Cpu*) {                                                                            // DivRemUF16(*rem, d1, d2)
        uint32_t rem = 0; c.r[0] = udiv(arg(1), arg(2), &rem); if (arg(0)) w32(arg(0), rem);
    };
    mv[-5] = [this, sdiv](Cpu*) { c.r[0] = uint32_t(sdiv(int32_t(arg(0)), int32_t(arg(1)), nullptr)); };    // DivSF16(d1, d2)
    mv[-6] = [this, sdiv](Cpu*) {                                                                            // DivRemSF16(*rem, d1, d2)
        int32_t rem = 0; c.r[0] = uint32_t(sdiv(int32_t(arg(1)), int32_t(arg(2)), &rem)); if (arg(0)) w32(arg(0), uint32_t(rem));
    };
    mv[-7] = [this, udiv](Cpu*) { c.r[0] = udiv(0x10000u, arg(0), nullptr); };                               // RecipUF16
    mv[-8] = [this, sdiv](Cpu*) { c.r[0] = uint32_t(sdiv(0x10000, int32_t(arg(0)), nullptr)); };            // RecipSF16
    make_folio("Operamath", kSubsysMath, 64, mv);
    install_math_swis();
    make_folio("International", kSubsysKernel, 32, {});
    make_folio("Compression", kSubsysKernel, 32, {});
    make_folio("JString", kSubsysKernel, 32, {});

    // Audio SWIs, named so the log reads as an implementation checklist.
    static const std::pair<uint32_t, const char*> audio[] = {
        {0, "TweakKnob"}, {1, "StartInstrument"}, {2, "ReleaseInstrument"}, {3, "StopInstrument"},
        {4, "TuneInsTemplate"}, {5, "TuneInstrument"}, {8, "ConnectInstruments"}, {9, "TraceAudio"},
        {10, "AllocAmplitude"}, {11, "FreeAmplitude"}, {12, "DisconnectInstruments"}, {13, "SignalAtTime"},
        {15, "SetAudioRate"}, {16, "SetAudioDuration"}, {17, "TweakRawKnob"}, {18, "StartAttachment"},
        {19, "ReleaseAttachment"}, {20, "StopAttachment"}, {21, "LinkAttachments"}, {22, "MonitorAttachment"},
        {23, "SetMasterTuning"}, {24, "AbandonInstrument"}, {25, "AdoptInstrument"}, {26, "ScavengeInstrument"},
        {27, "SetAudioItemInfo"}, {28, "PauseInstrument"}, {29, "ResumeInstrument"}, {30, "WhereAttachment"},
        {32, "BendInstrumentPitch"}, {33, "AbortTimerCue"}, {34, "EnableAudioInput"}, {36, "ReadProbe"},
        {38, "GetAudioFrameCount"}, {39, "GetAudioCyclesUsed"},
    };
    for (auto& [n, name] : audio) {
        std::string nm = std::string("audio:") + name;
        rt.register_swi(kAudioSwi + n, nm, [this, nm](Cpu*) { stub(nm); });
    }
    install_audio_swis();
    // SignalAtTime(cue, time): raise the cue's signal when the clock gets there.
    rt.register_swi(kAudioSwi + 13, "audio:SignalAtTime", [this](Cpu*) {
        const int32_t cue = int32_t(arg(0));
        timed_signals_.push_back({cur().item, cue_signal(cue), arg(1)});
        c.r[0] = 0;
    });
    rt.register_swi(kAudioSwi + 33, "audio:AbortTimerCue", [this](Cpu*) {
        const uint32_t mask = cue_signal(int32_t(arg(0)));
        timed_signals_.erase(std::remove_if(timed_signals_.begin(), timed_signals_.end(),
                                            [&](const TimedSignal& t) { return t.mask == mask; }), timed_signals_.end());
        c.r[0] = 0;
    });
}

// ============================================================================
// Math folio SWIs (operamath.h, MATHSWI = 0x50000): vector and matrix work in
// 16.16 fixed point. Vectors are rows multiplied on the left of row-major
// matrices: dest[i] = sum over j of v[j] * m[j][i]. Products are summed at 32.32
// and shifted once. Escape from Monster Manor builds its 3D view with these;
// without them every wall column came out at the same wrong place.
// ============================================================================
void Portfolio::install_math_swis() {
    auto rd = [this](uint32_t a) { return int32_t(r32(a)); };
    auto wr = [this](uint32_t a, int64_t v) { w32(a, uint32_t(int32_t(v))); };
    // vec (n) x mat (n x n) -> dest; dest may alias vec
    auto vecmat = [this, rd, wr](uint32_t dest, uint32_t vec, uint32_t mat, int n) {
        int64_t out[4] = {};
        for (int i = 0; i < n; ++i) {
            int64_t sum = 0;
            for (int j = 0; j < n; ++j) sum += int64_t(rd(vec + 4u * j)) * rd(mat + 4u * uint32_t(j * n + i));
            out[i] = sum >> 16;
        }
        for (int i = 0; i < n; ++i) wr(dest + 4u * i, out[i]);
    };
    auto matmat = [this, rd, wr](uint32_t dest, uint32_t a, uint32_t b, int n) {
        int64_t out[16] = {};
        for (int i = 0; i < n; ++i)
            for (int k = 0; k < n; ++k) {
                int64_t sum = 0;
                for (int j = 0; j < n; ++j) sum += int64_t(rd(a + 4u * uint32_t(i * n + j))) * rd(b + 4u * uint32_t(j * n + k));
                out[i * n + k] = sum >> 16;
            }
        for (int i = 0; i < n * n; ++i) wr(dest + 4u * uint32_t(i), out[i]);
    };
    auto divz = [this, rd, wr, vecmat](uint32_t dest, uint32_t vec, uint32_t mat, int32_t nn) {
        vecmat(dest, vec, mat, 3);
        const int64_t z = rd(dest + 8);
        if (z != 0) {
            wr(dest + 0, int64_t(rd(dest + 0)) * nn / z);
            wr(dest + 4, int64_t(rd(dest + 4)) * nn / z);
        }
    };
    auto stack_arg = [this](int k) { return r32(c.r[13] + 4u * uint32_t(k)); };   // 5th argument onwards
    auto M = [this](uint32_t n, const char* name, HleFn fn) { rt.register_swi(kMathSwi + n, name, std::move(fn)); };
    using std::sqrt;

    M(0, "MulVec3Mat33_F16", [this, vecmat](Cpu*) { vecmat(arg(0), arg(1), arg(2), 3); });
    M(1, "MulMat33Mat33_F16", [this, matmat](Cpu*) { matmat(arg(0), arg(1), arg(2), 3); });
    M(2, "MulManyVec3Mat33_F16", [this, vecmat](Cpu*) {
        for (uint32_t k = 0, n = arg(3); k < n && k < 100000; ++k) vecmat(arg(0) + 12 * k, arg(1) + 12 * k, arg(2), 3);
    });
    // objectlist[i] -> object; ObjOffset1: offsets of the dest-array pointer,
    // the source-array pointer, the matrix and the count inside the object.
    auto obj_vec = [this, vecmat](uint32_t list, uint32_t off, uint32_t count, int n) {
        const uint32_t od = r32(off), os = r32(off + 4), om = r32(off + 8), oc = r32(off + 12);
        for (uint32_t i = 0; i < count && i < 100000; ++i) {
            const uint32_t o = r32(list + 4 * i);
            if (!o) continue;
            const uint32_t d = r32(o + od), sv = r32(o + os), cnt = r32(o + oc);
            for (uint32_t k = 0; k < cnt && k < 100000; ++k)
                vecmat(d + uint32_t(4 * n) * k, sv + uint32_t(4 * n) * k, o + om, n);
        }
    };
    // ObjOffset2: offsets of the destination and source matrices in the object.
    auto obj_mat = [this, matmat](uint32_t list, uint32_t off, uint32_t mat, uint32_t count, int n) {
        const uint32_t od = r32(off), os = r32(off + 4);
        for (uint32_t i = 0; i < count && i < 100000; ++i) {
            const uint32_t o = r32(list + 4 * i);
            if (o) matmat(o + od, o + os, mat, n);
        }
    };
    M(3, "MulObjectVec3Mat33_F16", [this, obj_vec](Cpu*) { obj_vec(arg(0), arg(1), arg(2), 3); });
    M(4, "MulObjectMat33_F16", [this, obj_mat](Cpu*) { obj_mat(arg(0), arg(1), arg(2), arg(3), 3); });
    M(5, "MulManyF16", [this, rd, wr](Cpu*) {
        for (uint32_t k = 0, n = arg(3); k < n && k < 1000000; ++k)
            wr(arg(0) + 4 * k, (int64_t(rd(arg(1) + 4 * k)) * rd(arg(2) + 4 * k)) >> 16);
    });
    M(6, "MulScalarF16", [this, rd, wr](Cpu*) {
        const int64_t sc = int32_t(arg(2));
        for (uint32_t k = 0, n = arg(3); k < n && k < 1000000; ++k) wr(arg(0) + 4 * k, (int64_t(rd(arg(1) + 4 * k)) * sc) >> 16);
    });
    M(7, "MulVec4Mat44_F16", [this, vecmat](Cpu*) { vecmat(arg(0), arg(1), arg(2), 4); });
    M(8, "MulMat44Mat44_F16", [this, matmat](Cpu*) { matmat(arg(0), arg(1), arg(2), 4); });
    M(9, "MulManyVec4Mat44_F16", [this, vecmat](Cpu*) {
        for (uint32_t k = 0, n = arg(3); k < n && k < 100000; ++k) vecmat(arg(0) + 16 * k, arg(1) + 16 * k, arg(2), 4);
    });
    M(10, "MulObjectVec4Mat44_F16", [this, obj_vec](Cpu*) { obj_vec(arg(0), arg(1), arg(2), 4); });
    M(11, "MulObjectMat44_F16", [this, obj_mat](Cpu*) { obj_mat(arg(0), arg(1), arg(2), arg(3), 4); });
    M(12, "Dot3_F16", [this, rd](Cpu*) {
        int64_t s = 0; for (int j = 0; j < 3; ++j) s += int64_t(rd(arg(0) + 4u * j)) * rd(arg(1) + 4u * j);
        c.r[0] = uint32_t(int32_t(s >> 16));
    });
    M(13, "Dot4_F16", [this, rd](Cpu*) {
        int64_t s = 0; for (int j = 0; j < 4; ++j) s += int64_t(rd(arg(0) + 4u * j)) * rd(arg(1) + 4u * j);
        c.r[0] = uint32_t(int32_t(s >> 16));
    });
    M(14, "Cross3_F16", [this, rd, wr](Cpu*) {
        const int64_t a0 = rd(arg(1)), a1 = rd(arg(1) + 4), a2 = rd(arg(1) + 8);
        const int64_t b0 = rd(arg(2)), b1 = rd(arg(2) + 4), b2 = rd(arg(2) + 8);
        wr(arg(0), (a1 * b2 - a2 * b1) >> 16); wr(arg(0) + 4, (a2 * b0 - a0 * b2) >> 16); wr(arg(0) + 8, (a0 * b1 - a1 * b0) >> 16);
    });
    auto absvec = [this, rd](uint32_t v, int n) {
        double s = 0; for (int j = 0; j < n; ++j) { const double x = rd(v + 4u * j) / 65536.0; s += x * x; }
        return uint32_t(std::min(sqrt(s) * 65536.0, 4294967295.0));
    };
    M(15, "AbsVec3_F16", [this, absvec](Cpu*) { c.r[0] = absvec(arg(0), 3); });
    M(16, "AbsVec4_F16", [this, absvec](Cpu*) { c.r[0] = absvec(arg(0), 4); });
    M(17, "MulVec3Mat33DivZ_F16", [this, divz](Cpu*) { divz(arg(0), arg(1), arg(2), int32_t(arg(3))); });
    M(18, "MulManyVec3Mat33DivZ_F16", [this, divz](Cpu*) {    // (mmv3m33d*): dest, src, mat, n, count
        const uint32_t s = arg(0);
        const uint32_t d = r32(s), sv = r32(s + 4), m = r32(s + 8), cnt = r32(s + 16);
        const int32_t nn = int32_t(r32(s + 12));
        for (uint32_t k = 0; k < cnt && k < 100000; ++k) divz(d + 12 * k, sv + 12 * k, m, nn);
    });
    (void)stack_arg;
}

} // namespace tdort
