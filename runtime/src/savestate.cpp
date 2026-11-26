// savestate.cpp - save states: the whole machine between two frames.
//
// The game's tasks are register sets kept by the runtime (no host stacks), and
// everything else is guest memory or host-side bookkeeping, so a state is:
// the CPU, guest memory (with which translated code is still valid), the
// translated programs mapped at run time, the OS replacement (items, tasks,
// heaps, pending I/O, open files by name and position, timers, the event
// broker...), the audio engine and the cel engine's registers.
//
// A state is taken only between frames, where no OS call is half done.
// It fits only the converted game that wrote it (the LaunchMe hash is checked).
#include "hle_os.h"
#include "state_ar.h"
#include "tdort/gfx.h"
#include <cstdio>
#include <filesystem>

namespace fs = std::filesystem;

namespace tdort {

struct StateIO {
    static void io(StateAr& a, HleItem& it) {
        a(it.num); a(it.addr); a(it.size); a(it.subsys); a(it.type); a(it.kind); a(it.name); a(it.alive); a(it.heap_owned);
        a(it.guest_path); a(it.host_path); a(it.file_size); a(it.is_dir);
        // An open file: kept by name and position, opened again on load.
        bool open = it.fp != nullptr;
        int64_t at = open ? int64_t(std::ftell(it.fp)) : 0;
        a(open); a(at);
        a(it.device); a(it.reply_port); a(it.msg_item); a(it.msg_buffer); a(it.msg_buffer_size);
        a(it.queue); a(it.signal); a(it.in_port); a(it.children); a(it.buffer); a(it.owns_buffer); a(it.sem_owner); a(it.sem_count); a(it.sem_waiters); a(it.sem_handoff);
        a(it.owner_task); a(it.writable); a(it.block_size);
        if (!a.saving) {
            it.fp = nullptr;
            if (open && !it.host_path.empty()) {
#if defined(_WIN32)
                it.fp = _wfopen(fs::u8path(it.host_path).wstring().c_str(), it.writable ? L"r+b" : L"rb");
#else
                it.fp = std::fopen(it.host_path.c_str(), it.writable ? "r+b" : "rb");
#endif
                if (it.fp) std::fseek(it.fp, long(at), SEEK_SET);
            }
        }
    }
    static void io(StateAr& a, CallRec& c) { a(c.name); a(c.a); a(c.site); a(c.frame); a(c.count); }
    static void io(StateAr& a, HleTask& t) {
        a(t.item); a(t.addr); a(t.r); a(t.n); a(t.z); a(t.c); a(t.v); a(t.ctl); a(t.pending); a(t.allocated); a(t.pri);
        a(t.st); a(t.wake_frame); a(t.waiting); a(t.blocked_since); a(t.parent); a(t.owned_stack); a(t.program);
        a(t.image_lo); a(t.image_hi); a(t.past_sleep_frame); a(t.past_sleeps); a(t.exit_msg); a(t.exit_status);
        a(t.cwd); a(t.name); a(t.calls);
    }
    static void io(StateAr& a, Portfolio::ReadRec& r) { a(r.path); a(r.block); a(r.bytes); a(r.dest); a(r.frame); }
    static void io(StateAr& a, Portfolio::TemplateAttach& t) { a(t.item); a(t.sample); a(t.envelope); a(t.fifo); a(t.flags); a(t.start_at); }
    static void io(StateAr& a, Portfolio::DirList& d) { a(d.entries); a(d.next); }
    static void io(StateAr& a, Portfolio::CustomSampleMem& m) { a(m.free_fn); a(m.block); a(m.size); }
    static void io(StateAr& a, Portfolio::GuestCallback& g) { a(g.kind); a(g.ret); a(g.img); a(g.len); a(g.off); a(g.free_fn); a(g.sample); }
    static void io(StateAr& a, Portfolio::DiskStream& d) {
        a(d.size); a(d.pos); a(d.path);
        int64_t at = d.fp && a.saving ? int64_t(std::ftell(d.fp)) : 0;
        a(at);
        if (!a.saving) {
#if defined(_WIN32)
            d.fp = _wfopen(fs::u8path(d.path).wstring().c_str(), L"rb");
#else
            d.fp = std::fopen(d.path.c_str(), "rb");
#endif
            if (d.fp) std::fseek(d.fp, long(at), SEEK_SET);
        }
    }
    static void io(StateAr& a, Portfolio::VblIo& v) { a(v.ior); a(v.task); a(v.err); a(v.actual); a(v.due_frame); }
    static void io(StateAr& a, Portfolio::Phantom& p) { a(p.item); a(p.parent); a(p.end_frame); }
    static void io(StateAr& a, Portfolio::TimedSignal& t) { a(t.task); a(t.mask); a(t.at_ticks); }
    static void io(StateAr& a, Portfolio::Listener& l) { a(l.port); a(l.trigger); a(l.capture); a(l.fresh); a(l.held); }

    static void io(StateAr& a, Audio::Sample& s) {
        a(s.addr); a(s.bytes); a(s.frames); a(s.channels); a(s.bits); a(s.codec); a(s.rate); a(s.base_note); a(s.detune);
        a(s.sus_begin); a(s.sus_end); a(s.rel_begin); a(s.rel_end); a(s.host_alloc); a(s.data_offset); a(s.data_size);
    }
    static void io(StateAr& a, Audio::Envelope& e) { a(e.points); a(e.sus_begin); a(e.sus_end); }
    static void io(StateAr& a, Audio::Attachment& t) {
        a(t.ins); a(t.sample); a(t.envelope); a(t.fifo); a(t.flags); a(t.next); a(t.start_at); a(t.cue); a(t.cue_at);
        a(t.active); a(t.released); a(t.ended_flag); a(t.pos); a(t.dec_frame); a(t.dec_val); a(t.adp_index);
        a(t.duck_left); a(t.duck_skip); a(t.env_ms);
    }
    static void io(StateAr& a, Audio::Instrument& in) {
        a(in.tmpl); a(in.kind); a(in.fixed_rate); a(in.half_rate); a(in.stereo); a(in.silent); a(in.vpp); a(in.vpp_tab);
        a(in.freq_unit); a(in.data_bits); a(in.data_sdx2); a(in.data_adp4); a(in.data_duck); a(in.inputs); a(in.op);
        a(in.knobs); a(in.mod); a(in.env); a(in.dc_in); a(in.dc_out); a(in.st); a(in.paused); a(in.pitch_step);
        a(in.bend); a(in.phase); a(in.noise); a(in.atts); a(in.stamp); a(in.out_l); a(in.out_r); a(in.busy);
    }
    static void io(StateAr& a, Audio::Conn& c) { a(c.src); a(c.src_port); a(c.dst); a(c.dst_port); }
    static void io(StateAr& a, Gfx::Regs& r) {
        a(r.flags); a(r.pixc); a(r.pre0); a(r.pre1); a(r.xpos); a(r.ypos); a(r.hdx); a(r.hdy); a(r.vdx); a(r.vdy);
        a(r.hddx); a(r.hddy); a(r.plut);
    }

    static void io(StateAr& a, Audio& au) {
        a(au.ins_); a(au.samples_); a(au.envelopes_); a(au.atts_); a(au.conns_); a(au.stamp_); a(au.frames_out_);
        a(au.cues_fired); a(au.last_tweak);
    }
    static void io(StateAr& a, Gfx& g) {
        a(g.r_); a(g.display_lines_); a(g.display_addr_); a(g.clut_); a(g.lrform); a(g.cels_drawn); a(g.pixels_written);
        if (!a.saving) g.drop_shadows();   // rebuilt from the loaded bitmaps
    }
    static void io(StateAr& a, Portfolio& p) {
        // Files and streams open now are closed first: loading replaces them.
        if (!a.saving) {
            for (HleItem& it : p.items_) if (it.fp) { std::fclose(it.fp); it.fp = nullptr; }
            for (auto& kv : p.streams_) if (kv.second.fp) std::fclose(kv.second.fp);
            p.streams_.clear();
        }
        a(p.audio_rate_); a(p.audio_rate_ticks_); a(p.audio_rate_frame_); a(p.audio_clock_owner_); a(p.font_);
        p.codecs_io(a);
        a(p.deliver_code_); a(p.final_word_); a(p.last_input_frame_); a(p.recent_reads_);
        a(p.slice_start); a(p.mt);
        a(p.templates_); a(p.template_attach_); a(p.ins_template_); a(p.abandoned_); a(p.putc_loop_);
        a(p.knobs_); a(p.cue_owner_); a(p.knob_name_strings_); a(p.audio_device_);
        a(p.custom_sample_mem_); a(p.guest_callbacks_);
        a(p.vdl_buffer_); a(p.screen_vdl_); a(p.screen_vdl_buffer_); a(p.vdl_clut_); a(p.screen_default_vdl_); a(p.dirs_); a(p.streams_);
        a(p.last_load_bytes_); a(p.cd_free_pos_); a(p.vbl_io_);
        a(p.tasks_); a(p.cur_); a(p.blocked_streak_); a(p.thread_exit_trap_); a(p.idle_trap_); a(p.io_quick_); a(p.idle_);
        a(p.phantom_children_); a(p.timed_signals_); a(p.cue_signals_); a(p.cue_signal_task_);
        a(p.broker_port_); a(p.listeners_); a(p.broker_free_); a(p.broker_buf_); a(p.read_address_);
        a(p.disc_os_version_); a(p.locale_item_); a(p.broker_focus_);
        a(p.disc_);
        a(p.broker_dest_); a(p.last_pad_); a(p.frame_no_); a(p.hw_random_);
        a(p.kbase_); a(p.memhdr_dram_); a(p.memhdr_vram_); a(p.graphics_base_);
        a(p.items_); a(p.stub_counts_); a(p.free_dram_); a(p.free_vram_); a(p.allocs_); a(p.quiet_alloc_);
        a(p.alloc_owner_); a(p.aliases_);
    }
    static void io(StateAr& a, Runtime::LoadedModule& m) {
        // The translated program by its position in the table of this build.
        uint32_t idx = 0xFFFFFFFFu;
        if (a.saving) for (uint32_t i = 0; i < tdort_module_count; ++i) if (&tdort_modules[i] == m.info) idx = i;
        a(idx);
        if (!a.saving) m.info = idx < tdort_module_count ? &tdort_modules[idx] : nullptr;
        a(m.base); a(m.size); a(m.entry_pc); a(m.active); a(m.lookup); a(m.name);
    }
    static void io(StateAr& a, Runtime& rt) {
        Cpu& c = rt.cpu;
        a(c.r); a(c.n); a(c.z); a(c.c); a(c.v); a(c.ctl); a(c.cycles); a(c.halted); a(c.mod_base); a(c.blk_base);
        a(rt.mem.ram); a(rt.mem.code_granule); a(rt.mem.granule_dirty); a(rt.mem.dirty_by);
        if (!a.saving) std::fill(rt.mem.dirty_call.begin(), rt.mem.dirty_call.end(), nullptr);
        a(rt.modules_);
        a(rt.hle_code_lo_); a(rt.hle_code_hi_); a(rt.hle_heap_); a(rt.hle_heap_end_);
        a(rt.retry_pc); a(rt.frames_); a(rt.vblank_wait_); a(rt.entered_); a(rt.entry_);
        a(rt.screen_hash_); a(rt.screen_change_frame_); a(rt.io_seen_);
        io(a, *rt.os); io(a, *rt.audio); io(a, *rt.gfx);
    }
};

template <class T> void StateAr::io_struct(T& v) { StateIO::io(*this, v); }

// File layout: magic, version, the game's LaunchMe hash, the frame, then the data.
static constexpr char kMagic[8] = {'3', 'D', 'O', 'S', 'T', 'A', 'T', 'E'};
static constexpr uint32_t kVersion = 8;

static uint64_t checksum(const uint8_t* p, size_t n) {
    uint64_t h = 1469598103934665603ull;
    for (size_t i = 0; i < n; ++i) h = (h ^ p[i]) * 1099511628211ull;
    return h;
}

// Which translation this build carries: the main program's block table and the
// translated programs. A re-conversion that translated differently refuses the
// older states (their record of where translated code lies would not match).
static std::string build_id() {
    uint64_t h = 1469598103934665603ull;
    auto mix = [&](uint64_t v) { h = (h ^ v) * 1099511628211ull; };
    mix(tdort_recomp_count);
    for (uint32_t i = 0; i < tdort_recomp_count; i += 97) { mix(tdort_recomp_table[i].guest_addr); mix(tdort_recomp_table[i].byte_len); }
    mix(tdort_module_count);
    for (uint32_t i = 0; i < tdort_module_count; ++i) {
        mix(tdort_modules[i].count);
        for (const char* p = tdort_modules[i].file_hash; p && *p; ++p) mix(uint8_t(*p));
    }
    char buf[40]; std::snprintf(buf, sizeof buf, "%s/%016llx", tdort_image_hash, (unsigned long long)h);
    return buf;
}

bool Runtime::save_state(const std::string& path, std::string& err) {
    StateAr a;
    a.saving = true;
    a.buf.reserve(mem.ram.size() + (1u << 20));
    std::string hash = build_id();
    char magic[8]; std::memcpy(magic, kMagic, 8);
    uint32_t version = kVersion, frame = frames_;
    a(magic); a(version); a(hash); a(frame);
    // Room for the length and checksum of what follows, filled in below.
    const size_t sum_at = a.buf.size();
    uint64_t len = 0, sum = 0;
    a(len); a(sum);
    StateIO::io(a, *this);
    len = a.buf.size() - (sum_at + 16);
    sum = checksum(a.buf.data() + sum_at + 16, size_t(len));
    std::memcpy(&a.buf[sum_at], &len, 8); std::memcpy(&a.buf[sum_at + 8], &sum, 8);
    std::error_code ec;
    fs::create_directories(fs::u8path(path).parent_path(), ec);
    const std::string tmp = path + ".tmp";
    FILE* f = std::fopen(tmp.c_str(), "wb");
    if (!f) { err = "can't write " + path; return false; }
    const bool ok = std::fwrite(a.buf.data(), 1, a.buf.size(), f) == a.buf.size();
    std::fclose(f);
    if (!ok) { err = "disk full?"; fs::remove(fs::u8path(tmp), ec); return false; }
    fs::rename(fs::u8path(tmp), fs::u8path(path), ec);        // a crash mid-write keeps the old state
    if (ec) { fs::remove(fs::u8path(path), ec); fs::rename(fs::u8path(tmp), fs::u8path(path), ec); }
    log("save state: %s (frame %u, %zu bytes)", path.c_str(), frames_, a.buf.size());
    return !ec;
}

bool Runtime::load_state(const std::string& path, std::string& err) {
    StateAr a;
    a.saving = false;
    {
        FILE* f = std::fopen(path.c_str(), "rb");
        if (!f) { err = "empty slot"; return false; }
        std::fseek(f, 0, SEEK_END);
        const long n = std::ftell(f);
        std::fseek(f, 0, SEEK_SET);
        a.buf.resize(n > 0 ? size_t(n) : 0);
        const bool ok = n > 0 && std::fread(a.buf.data(), 1, a.buf.size(), f) == a.buf.size();
        std::fclose(f);
        if (!ok) { err = "can't read the file"; return false; }
    }
    char magic[8] = {}; uint32_t version = 0, frame = 0; std::string hash;
    a(magic); a(version); a(hash); a(frame);
    if (a.bad || std::memcmp(magic, kMagic, 8) != 0) { err = "not a save state"; return false; }
    if (version != kVersion) { err = "made by another version of 3DO to PC"; return false; }
    if (hash != build_id()) { err = "made by another game or conversion"; return false; }
    uint64_t len = 0, sum = 0;
    a(len); a(sum);
    if (a.bad || len != a.buf.size() - a.pos || checksum(a.buf.data() + a.pos, size_t(len)) != sum) {
        err = "the file is damaged"; return false;
    }
    // Everything is checked before the machine is touched, so read into it now.
    StateIO::io(a, *this);
    if (a.bad) {
        // A damaged file after the header: the machine is half replaced, so stop.
        crash_message = "The save state " + path + " is damaged; the game can't continue from it.";
        crashed = true; cpu.halted = true;
        err = "damaged file";
        return false;
    }
    log("load state: %s (frame %u)", path.c_str(), frame);
    return true;
}

} // namespace tdort
